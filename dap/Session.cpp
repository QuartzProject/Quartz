#include "Session.hpp"

#include <algorithm>
#include <filesystem>

namespace quartz::dap {

namespace {

std::string canonical(const std::string& path) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : p.string();
}

Json capabilities() {
    Json filters = Json::array();
    filters.push(Json::object()
                     .set("filter", "trap")
                     .set("label", "Traps")
                     .set("description", "Stop where a trap (panic, failed check, out of fuel...) ends the program")
                     .set("default", true));
    return Json::object()
        .set("supportsConfigurationDoneRequest", true)
        .set("supportsConditionalBreakpoints", true)
        .set("supportsEvaluateForHovers", true)
        .set("supportsExceptionInfoRequest", true)
        .set("supportsTerminateRequest", true)
        .set("exceptionBreakpointFilters", std::move(filters));
}

} // namespace

int runDebugAdapter(const DebugLoader& load) { return Session(load).run(); }

int Session::run() {
    while (!disconnected_) {
        std::optional<std::string> body = transport_.read();
        if (!body) {
            break;
        }
        if (std::optional<Json> msg = Json::parse(*body); msg && msg->isObject()) {
            handle(*msg);
        }
    }
    {
        std::lock_guard lock(mutex_);
        terminate_ = true;
        if (state_ == State::Stopped) {
            state_ = State::Running;
        }
    }
    cv_.notify_all();
    if (program_.joinable()) {
        program_.join();
    }
    return 0;
}

void Session::send(Json msg) {
    std::lock_guard lock(writeMutex_);
    msg.set("seq", seq_++);
    transport_.write(msg.dump());
}

void Session::respond(const Json& request, Json body, bool success, std::string_view message) {
    Json r = Json::object()
                 .set("type", "response")
                 .set("request_seq", request["seq"].integer())
                 .set("command", request["command"].str())
                 .set("success", success);
    if (!message.empty()) {
        r.set("message", message);
    }
    r.set("body", std::move(body));
    send(std::move(r));
}

void Session::event(std::string_view name, Json body) {
    send(Json::object().set("type", "event").set("event", name).set("body", std::move(body)));
}

void Session::handle(const Json& msg) {
    if (msg["type"].str() != "request") {
        return;
    }
    std::string_view cmd = msg["command"].str();
    const Json& args = msg["arguments"];
    if (cmd == "initialize") {
        respond(msg, capabilities());
    } else if (cmd == "launch") {
        launch(msg);
    } else if (cmd == "setBreakpoints") {
        setBreakpoints(msg);
    } else if (cmd == "setExceptionBreakpoints") {
        bool trap = false;
        for (const Json& f : args["filters"].items()) {
            trap = trap || f.str() == "trap";
        }
        {
            std::lock_guard lock(mutex_);
            stopOnTrap_ = trap;
        }
        respond(msg, Json::object().set("breakpoints", Json::array()));
    } else if (cmd == "setFunctionBreakpoints") {
        respond(msg, Json::object().set("breakpoints", Json::array()));
    } else if (cmd == "configurationDone") {
        respond(msg, Json::object());
        std::lock_guard lock(mutex_);
        configured_ = true;
        maybeStart();
    } else if (cmd == "threads") {
        std::lock_guard lock(mutex_);
        Json threads = Json::array();
        threads.push(Json::object().set("id", 1).set("name", threadName_));
        respond(msg, Json::object().set("threads", std::move(threads)));
    } else if (cmd == "pause") {
        pause_ = true;
        respond(msg, Json::object());
    } else if (cmd == "continue" || cmd == "next" || cmd == "stepIn" || cmd == "stepOut") {
        std::unique_lock lock(mutex_);
        if (!waitForStop(lock)) {
            lock.unlock();
            respond(msg, Json::object(), false, "the program is not stopped");
            return;
        }
        Step step = cmd == "next" ? Step::Over : cmd == "stepIn" ? Step::In : cmd == "stepOut" ? Step::Out : Step::None;
        lock.unlock();
        respond(msg, cmd == "continue" ? Json::object().set("allThreadsContinued", true) : Json::object());
        resume(step);
    } else if (cmd == "stackTrace" || cmd == "scopes" || cmd == "variables" || cmd == "evaluate" ||
               cmd == "exceptionInfo") {
        std::unique_lock lock(mutex_);
        if (!waitForStop(lock)) {
            lock.unlock();
            respond(msg, Json::object(), false, "the program is not stopped");
            return;
        }
        if (cmd == "stackTrace") {
            Json body = stackTrace(args);
            lock.unlock();
            respond(msg, std::move(body));
        } else if (cmd == "scopes") {
            Json body = scopes(args);
            lock.unlock();
            respond(msg, std::move(body));
        } else if (cmd == "variables") {
            Json body = variables(args);
            lock.unlock();
            respond(msg, std::move(body));
        } else if (cmd == "evaluate") {
            auto r = evaluate(args);
            lock.unlock();
            respond(msg, r ? std::move(*r) : Json::object(), r.has_value(), r ? "" : r.error());
        } else {
            Json body = Json::object()
                            .set("exceptionId", stopReason_ == "exception" ? "trap" : stopReason_)
                            .set("description", stopText_)
                            .set("breakMode", "always");
            lock.unlock();
            respond(msg, std::move(body));
        }
    } else if (cmd == "disconnect" || cmd == "terminate") {
        {
            std::lock_guard lock(mutex_);
            terminate_ = true;
            if (state_ == State::Stopped) {
                state_ = State::Running;
                stopped_ = nullptr;
                frames_.clear();
                refs_.clear();
            }
        }
        cv_.notify_all();
        if (cmd == "disconnect" && program_.joinable()) {
            program_.join();
        }
        respond(msg, Json::object());
        disconnected_ = cmd == "disconnect";
    } else {
        respond(msg, Json::object(), false, "unsupported request");
    }
}

void Session::launch(const Json& request) {
    const Json& args = request["arguments"];
    std::string program(args["program"].str());
    if (program.empty()) {
        respond(request, Json::object(), false, "launch needs a 'program': a .qz file or a package folder");
        return;
    }
    auto target = load_(program);
    if (!target) {
        output("stderr", target.error());
        respond(request, Json::object(), false, target.error());
        return;
    }
    if (!target->diagnostics.empty()) {
        output("stderr", target->diagnostics);
    }
    {
        std::unique_lock lock(mutex_);
        target_ = std::move(*target);
        if (args.has("entry")) {
            entry_ = std::string(args["entry"].str());
        }
        tests_ = args["tests"].boolean();
        filter_ = std::string(args["filter"].str());
        noDebug_ = args["noDebug"].boolean();
        threadName_ = tests_ ? "tests" : entry_;
        step_ = args["stopOnEntry"].boolean() && !noDebug_ ? Step::In : Step::None;
        const vm::Program& p = *target_->program;
        fnFile_.clear();
        for (const vm::CompiledFunction& f : p.functions) {
            fnFile_.push_back(f.file.empty() || f.file.starts_with("<") ? std::string() : canonical(f.file));
        }
        resolveBreakpoints(lock);
        launched_ = true;
    }
    respond(request, Json::object());
    event("initialized", Json::object());
    std::lock_guard lock(mutex_);
    maybeStart();
}

void Session::maybeStart() {
    if (!launched_ || !configured_ || started_) {
        return;
    }
    started_ = true;
    state_ = State::Running;
    program_ = std::thread([this] { programThread(); });
}

Json Session::resolve(const Requested& req) {
    const vm::Program& p = *target_->program;
    std::string file = canonical(req.path);
    Json out = Json::array();
    auto inFile = [&](const vm::ProbeSite& site) {
        return site.kind == vm::ProbeKind::Line && site.function < fnFile_.size() && fnFile_[site.function] == file;
    };
    for (const auto& [line, condition] : req.lines) {
        std::uint32_t best = 0;
        for (const vm::ProbeSite& site : p.probeSites) {
            if (inFile(site) && site.line >= line && (best == 0 || site.line < best)) {
                best = site.line;
            }
        }
        std::int64_t id = nextBreakpointId_++;
        Json bp = Json::object().set("id", id).set("verified", best != 0);
        if (best == 0) {
            out.push(std::move(bp.set("line", line).set("message", "no code on or after this line")));
            continue;
        }
        for (const vm::ProbeSite& site : p.probeSites) {
            std::vector<Breakpoint>& list = fnBreakpoints_[site.function < fnBreakpoints_.size() ? site.function : 0];
            bool known = std::any_of(list.begin(), list.end(), [&](const Breakpoint& b) { return b.id == id; });
            if (inFile(site) && site.line == best && !known) {
                list.push_back({best, condition, id});
            }
        }
        out.push(std::move(bp.set("line", best).set("source", Json::object().set("path", req.path))));
    }
    return out;
}

void Session::resolveBreakpoints(std::unique_lock<std::mutex>&) {
    if (!target_) {
        return;
    }
    fnBreakpoints_.assign(target_->program->functions.size(), {});
    for (const Requested& r : requested_) {
        resolve(r);
    }
}

void Session::setBreakpoints(const Json& request) {
    const Json& args = request["arguments"];
    Requested req;
    req.path = std::string(args["source"]["path"].str());
    for (const Json& b : args["breakpoints"].items()) {
        std::int64_t line = b["line"].integer();
        if (line > 0 && line <= 0xFFFFFFFF) {
            req.lines.emplace_back(static_cast<std::uint32_t>(line), std::string(b["condition"].str()));
        }
    }
    Json result = Json::array();
    {
        std::unique_lock lock(mutex_);
        std::erase_if(requested_, [&](const Requested& r) { return r.path == req.path; });
        requested_.push_back(req);
        if (target_) {
            fnBreakpoints_.assign(target_->program->functions.size(), {});
            for (const Requested& r : requested_) {
                Json one = resolve(r);
                if (r.path == req.path) {
                    result = std::move(one);
                }
            }
        } else {
            for (const auto& [line, condition] : req.lines) {
                result.push(Json::object().set("verified", false).set("line", line).set("message", "not loaded yet"));
            }
        }
    }
    respond(request, Json::object().set("breakpoints", std::move(result)));
}

bool Session::waitForStop(std::unique_lock<std::mutex>& lock) {
    cv_.wait(lock, [&] { return state_ != State::Running; });
    return state_ == State::Stopped;
}

void Session::resume(Step step) {
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::Stopped) {
            return;
        }
        step_ = step;
        stepDepth_ = frames_.empty() ? 0 : frames_.size() - 1;
        stopped_ = nullptr;
        frames_.clear();
        refs_.clear();
        state_ = State::Running;
    }
    cv_.notify_all();
}

} // namespace quartz::dap
