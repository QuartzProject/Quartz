#include "Scheduler.hpp"
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
    std::vector<std::uint32_t> functions;
    std::vector<std::string> names{entry_};
    names.insert(names.end(), tasks_.begin(), tasks_.end());
    for (const std::string& name : names) {
        auto fn = program->lookupFunction(name);
        if (!fn) {
            output("stderr", fn.error() == vm::LookupError::Ambiguous
                                 ? std::format("several functions are called '{}'; add their package\n", name)
                                 : std::format("no function '{}'\n", name));
            return 2;
        }
        functions.push_back(*fn);
    }
    return runTasks(sb, functions);
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
        vm::Sandbox sb(program);
        std::shared_ptr<void> keep;
        std::optional<std::string> err = prepare(sb, keep);
        std::expected<void, vm::Trap> r;
        if (err) {
            r = std::unexpected(vm::Trap{ir::TrapCode::HostError, *err, {}, 0});
        } else {
            {
                std::lock_guard lock(mutex_);
                spawning_ = name;
            }
            TickScheduler scheduler(sb);
            auto t = scheduler.spawn(a.function);
            while (t && !terminate_ && t->kind != vm::TaskOutcome::Kind::Done) {
                t = *scheduler.next();
            }
            scheduler.cancelAll();
            std::lock_guard lock(mutex_);
            spawning_.clear();
            endThreads(sb);
            if (!t) {
                r = std::unexpected(t.error());
            }
        }
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
    current_ = threadOf(sandbox, sandbox.probeTask());
    shown_.assign(1, Shown{current_, 0, frames_.size()});
    Json body = Json::object().set("reason", stopReason_).set("threadId", current_).set("allThreadsStopped", true);
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
        if (b.line == event.line && b.column == event.column && (b.condition.empty() || conditionHolds(b.condition))) {
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
        if (stopsOnTrap(sandbox.trap_.code) && !terminate_) {
            stopTrap_ = sandbox.trap_.code;
            stop(sandbox, "exception", sandbox.trap_.describe(), lock);
        }
        return true;
    }
    if (event.kind != vm::ProbeEvent::Kind::Line && event.kind != vm::ProbeEvent::Kind::Enter) {
        return true;
    }
    std::unique_lock lock(mutex_);
    std::int64_t thread = threadOf(sandbox, sandbox.probeTask());
    if (event.kind == vm::ProbeEvent::Kind::Enter || !armed_.empty()) {
        std::optional<std::string> entry = functionEntry(sandbox, event, thread);
        if (entry) {
            step_ = Step::None;
            stop(sandbox, std::move(*entry), {}, lock);
            return !terminate_;
        }
        if (event.kind == vm::ProbeEvent::Kind::Enter) {
            return true;
        }
    }
    std::string reason;
    bool stepping = step_ != Step::None && thread == stepThread_;
    if (pause_.exchange(false)) {
        reason = "pause";
    } else if (stepping && (step_ == Step::In || (step_ == Step::Over && event.depth <= stepDepth_) ||
                            (step_ == Step::Out && event.depth < stepDepth_))) {
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
