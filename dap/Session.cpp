#include "Session.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace quartz::dap {

namespace {

std::string canonical(const std::string& path) {
    if (path.starts_with("<")) {
        return path;
    }
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : p.string();
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
        respond(msg, adapterCapabilities());
    } else if (cmd == "launch") {
        launch(msg);
    } else if (cmd == "setBreakpoints") {
        setBreakpoints(msg);
    } else if (cmd == "setExceptionBreakpoints") {
        setExceptionBreakpoints(msg);
    } else if (cmd == "setFunctionBreakpoints") {
        setFunctionBreakpoints(msg);
    } else if (cmd == "source") {
        auto r = librarySourceText(args);
        respond(msg, r ? std::move(*r) : Json::object(), r.has_value(), r ? "" : r.error());
    } else if (cmd == "configurationDone") {
        respond(msg, Json::object());
        std::lock_guard lock(mutex_);
        configured_ = true;
        maybeStart();
    } else if (cmd == "threads") {
        std::unique_lock lock(mutex_);
        Json threads = threadList();
        lock.unlock();
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
        std::int64_t thread = args.has("threadId") ? args["threadId"].integer() : current_;
        lock.unlock();
        respond(msg, cmd == "continue" ? Json::object().set("allThreadsContinued", true) : Json::object());
        resume(step, thread);
    } else if (cmd == "stackTrace" || cmd == "scopes" || cmd == "variables" || cmd == "evaluate" ||
               cmd == "exceptionInfo" || cmd == "setVariable" || cmd == "setExpression") {
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
        } else if (cmd == "evaluate" || cmd == "setVariable" || cmd == "setExpression") {
            auto r = cmd == "evaluate"      ? evaluate(args)
                     : cmd == "setVariable" ? setVariable(args)
                                            : setExpression(args);
            lock.unlock();
            respond(msg, r ? std::move(*r) : Json::object(), r.has_value(), r ? "" : r.error());
        } else {
            Json body =
                Json::object()
                    .set("exceptionId", stopReason_ == "exception" ? std::string(trapFilterOf(stopTrap_)) : stopReason_)
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
                shown_.clear();
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
        tasks_.clear();
        for (const Json& t : args["tasks"].items()) {
            if (!t.str().empty()) {
                tasks_.emplace_back(t.str());
            }
        }
        step_ = args["stopOnEntry"].boolean() && !noDebug_ ? Step::In : Step::None;
        const vm::Program& p = *target_->program;
        fnFile_.clear();
        for (const vm::CompiledFunction& f : p.functions) {
            fnFile_.push_back(f.file.empty() ? std::string() : canonical(f.file));
        }
        resolveBreakpoints(lock);
        resolveFunctions();
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
        Json source = Json::object().set("path", req.path);
        if (std::int64_t ref = librarySource(req.path); ref != 0) {
            source = Json::object().set("name", req.path.substr(req.path.rfind('/') + 1)).set("sourceReference", ref);
        }
        out.push(std::move(bp.set("line", best).set("source", std::move(source))));
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
    if (req.path.empty()) {
        req.path = std::string(librarySourceName(args["source"]["sourceReference"].integer()));
    }
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

// Waits for the next stop, bounded so an evaluation sent while the program runs cannot hold up a `pause`.
bool Session::waitForStop(std::unique_lock<std::mutex>& lock) {
    cv_.wait_for(lock, std::chrono::seconds(5), [&] { return state_ != State::Running; });
    return state_ == State::Stopped;
}

void Session::resume(Step step, std::int64_t thread) {
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::Stopped) {
            return;
        }
        std::optional<Shown> frames = framesOf(thread);
        step_ = frames && frames->count > 0 ? step : Step::None;
        stepThread_ = thread;
        stepDepth_ = frames && frames->count > 0 ? frames->count - 1 : 0;
        stopped_ = nullptr;
        frames_.clear();
        shown_.clear();
        refs_.clear();
        state_ = State::Running;
    }
    cv_.notify_all();
}

} // namespace quartz::dap
