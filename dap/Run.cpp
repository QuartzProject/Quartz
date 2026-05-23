#include "Session.hpp"

#include <algorithm>
#include <format>

namespace quartz::dap {

namespace {

bool probeThunk(vm::Sandbox& sandbox, const vm::ProbeEvent& event, void* user) {
    return static_cast<Session*>(user)->onProbe(sandbox, event);
}

std::string_view text(const vm::Slot& s) { return static_cast<const vm::Str*>(s.p)->view(); }

bool hostPrint(vm::HostCall& call, void* user) {
    static_cast<Session*>(user)->output("stdout", std::string(text(call.args[0])) + "\n");
    return true;
}

bool hostTicks(vm::HostCall& call, void*) {
    call.result.u = call.args[0].u;
    return true;
}

bool hostLog(vm::HostCall& call, void* user) {
    static constexpr const char* kLevels[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    std::string line = std::string("[") + kLevels[std::clamp<std::int64_t>(call.args[0].i, 0, 4)] + "] ";
    line.append(text(call.args[1])).append(": ").append(text(call.args[2]));
    const auto* fields = static_cast<const vm::List*>(call.args[3].p);
    for (std::uint32_t i = 0; i + 1 < fields->len; i += 2) {
        line.append(" ").append(text(fields->data[i])).append("=").append(text(fields->data[i + 1]));
    }
    static_cast<Session*>(user)->output("stderr", line + "\n");
    return true;
}

} // namespace

void Session::output(std::string_view category, std::string text) {
    event("output", Json::object().set("category", category).set("output", std::move(text)));
}

void Session::bindHost(vm::Sandbox& sb) {
    sb.bind("print", hostPrint, this);
    sb.bind("int.ticks", hostTicks);
    sb.bind("org.quartzlang.log.emit", hostLog, this);
}

std::optional<std::string> Session::prepare(vm::Sandbox& sb, std::shared_ptr<void>& keep) {
    bindHost(sb);
    if (target_->link) {
        auto linked = target_->link(sb, [this](vm::Sandbox& s) { bindHost(s); });
        if (!linked) {
            return linked.error();
        }
        keep = std::move(*linked);
    }
    if (!noDebug_) {
        sb.setProbe(probeThunk, this);
    }
    if (auto s = sb.start(); !s) {
        return s.error().describe();
    }
    return std::nullopt;
}

void Session::programThread() {
    int code = tests_ ? runTests() : runMain();
    {
        std::lock_guard lock(mutex_);
        exitCode_ = code;
        state_ = State::Terminated;
    }
    cv_.notify_all();
    event("exited", Json::object().set("exitCode", code));
    event("terminated", Json::object());
}

int Session::runMain() {
    const std::shared_ptr<const vm::Program>& program = target_->program;
    vm::Sandbox sb(program);
    std::shared_ptr<void> keep;
    if (auto err = prepare(sb, keep)) {
        output("stderr", *err + "\n");
        return 3;
    }
    std::uint32_t fn = program->findFunction(entry_);
    if (fn == ~0U) {
        output("stderr", std::format("no function '{}'\n", entry_));
        return 2;
    }
    auto t = sb.spawn(fn, {});
    while (t && t->kind != vm::TaskOutcome::Kind::Done && !terminate_) {
        t = sb.resume(t->task);
    }
    if (terminate_) {
        return 0;
    }
    if (!t) {
        output("stderr", t.error().describe() + "\n");
        return 3;
    }
    if (program->functions[fn].result == ir::kI64) {
        output("stdout", std::format("{}\n", t->value.i));
    }
    return 0;
}

int Session::runTests() {
    const std::shared_ptr<const vm::Program>& program = target_->program;
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const ir::Annotation& a : program->module->annotations) {
        const std::string& name = program->functions[a.function].name;
        if (a.name != "test" || (!filter_.empty() && name.find(filter_) == std::string::npos) || terminate_) {
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            threadName_ = name;
        }
        vm::Sandbox sb(program);
        std::shared_ptr<void> keep;
        std::optional<std::string> err = prepare(sb, keep);
        auto r =
            err ? std::expected<vm::Slot, vm::Trap>(std::unexpected(vm::Trap{ir::TrapCode::HostError, *err, {}, 0}))
                : sb.call(a.function, {});
        if (terminate_) {
            break;
        }
        if (r) {
            ++passed;
            output("console", std::format("ok      {}\n", name));
        } else {
            ++failed;
            output("console", std::format("FAILED  {}: {}\n", name, r.error().describe()));
        }
    }
    output("console", std::format("\n{} passed, {} failed\n", passed, failed));
    return failed == 0 ? 0 : 1;
}

void Session::stop(vm::Sandbox& sandbox, std::string reason, std::string text, std::unique_lock<std::mutex>& lock) {
    state_ = State::Stopped;
    stopped_ = &sandbox;
    frames_ = sandbox.frames();
    refs_.clear();
    stopReason_ = std::move(reason);
    stopText_ = std::move(text);
    Json body = Json::object().set("reason", stopReason_).set("threadId", 1).set("allThreadsStopped", true);
    if (!stopText_.empty()) {
        body.set("description", stopReason_ == "exception" ? "Trapped" : stopText_).set("text", stopText_);
    }
    event("stopped", std::move(body));
    cv_.notify_all();
    cv_.wait(lock, [&] { return state_ != State::Stopped; });
}

bool Session::breakpointAt(const vm::ProbeEvent& event) {
    if (event.function >= fnBreakpoints_.size()) {
        return false;
    }
    for (const Breakpoint& b : fnBreakpoints_[event.function]) {
        if (b.line == event.line && (b.condition.empty() || conditionHolds(b.condition))) {
            return true;
        }
    }
    return false;
}

bool Session::onProbe(vm::Sandbox& sandbox, const vm::ProbeEvent& event) {
    if (terminate_) {
        return false;
    }
    if (event.kind == vm::ProbeEvent::Kind::Trap) {
        std::unique_lock lock(mutex_);
        if (stopOnTrap_ && !terminate_) {
            stop(sandbox, "exception", sandbox.trap_.describe(), lock);
        }
        return true;
    }
    if (event.kind != vm::ProbeEvent::Kind::Line) {
        return true;
    }
    std::unique_lock lock(mutex_);
    std::string reason;
    if (pause_.exchange(false)) {
        reason = "pause";
    } else if (step_ == Step::In || (step_ == Step::Over && event.depth <= stepDepth_) ||
               (step_ == Step::Out && event.depth < stepDepth_)) {
        reason = "step";
    } else {
        // Conditions read the frame the probe stopped in.
        frames_ = sandbox.frames();
        if (breakpointAt(event)) {
            reason = "breakpoint";
        }
        frames_.clear();
        refs_.clear();
    }
    if (reason.empty()) {
        return true;
    }
    step_ = Step::None;
    stop(sandbox, std::move(reason), {}, lock);
    return !terminate_;
}

} // namespace quartz::dap
