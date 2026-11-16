#include "Session.hpp"
#include "vm/Scheduler.hpp"

#include <algorithm>
#include <format>

namespace quartz::dap {

std::int64_t Session::threadOf(const vm::Sandbox& sandbox, std::optional<std::uint32_t> task) {
    for (const Thread& t : threads_) {
        if (t.sandbox == &sandbox && t.task == task) {
            return t.id;
        }
    }
    std::string name = spawning_.empty() ? std::format("task {}", task.value_or(0)) : std::exchange(spawning_, {});
    Thread t{nextThread_++, &sandbox, task, std::move(name)};
    event("thread", Json::object().set("reason", "started").set("threadId", t.id));
    threads_.push_back(std::move(t));
    return threads_.back().id;
}

void Session::endThread(const vm::Sandbox& sandbox, std::optional<std::uint32_t> task) {
    auto it = std::find_if(threads_.begin(), threads_.end(),
                           [&](const Thread& t) { return t.sandbox == &sandbox && t.task == task; });
    if (it != threads_.end()) {
        event("thread", Json::object().set("reason", "exited").set("threadId", it->id));
        threads_.erase(it);
    }
}

void Session::endThreads(const vm::Sandbox& sandbox) {
    while (true) {
        auto it =
            std::find_if(threads_.begin(), threads_.end(), [&](const Thread& t) { return t.sandbox == &sandbox; });
        if (it == threads_.end()) {
            return;
        }
        endThread(sandbox, it->task);
    }
}

Json Session::threadList() {
    Json out = Json::array();
    for (const Thread& t : threads_) {
        out.push(Json::object().set("id", t.id).set("name", t.name));
    }
    if (threads_.empty() && state_ != State::Terminated) {
        out.push(Json::object().set("id", nextThread_).set("name", tests_ ? "tests" : entry_));
    }
    return out;
}

// The stopped thread's frames come first; another task's are read from its saved frames on demand.
std::optional<Session::Shown> Session::framesOf(std::int64_t thread) {
    for (const Shown& s : shown_) {
        if (s.thread == thread) {
            return s;
        }
    }
    auto it = std::find_if(threads_.begin(), threads_.end(), [&](const Thread& t) { return t.id == thread; });
    if (it == threads_.end() || stopped_ == nullptr || it->sandbox != stopped_ || !it->task) {
        return std::nullopt;
    }
    std::vector<vm::FrameView> frames = stopped_->taskFrames(*it->task);
    Shown s{thread, frames_.size(), frames.size()};
    frames_.insert(frames_.end(), frames.begin(), frames.end());
    shown_.push_back(s);
    return s;
}

int Session::runTasks(vm::Sandbox& sb, const std::vector<std::uint32_t>& functions) {
    const vm::Program& program = sb.program();
    vm::TickScheduler scheduler(sb);
    auto key = [&](std::uint32_t fn, const vm::TaskOutcome& t) {
        return program.functions[fn].isSuspend ? std::optional<std::uint32_t>(t.task) : std::nullopt;
    };
    auto failed = [&](const vm::Trap& trap) {
        scheduler.cancelAll();
        std::lock_guard lock(mutex_);
        endThreads(sb);
        if (!terminate_) {
            output("stderr", trap.describe() + "\n");
        }
        return 3;
    };
    std::optional<std::uint32_t> entryTask;
    std::optional<vm::Slot> result;
    for (std::size_t i = 0; i < functions.size() && !terminate_; ++i) {
        std::uint32_t fn = functions[i];
        {
            std::lock_guard lock(mutex_);
            spawning_ = i == 0 ? entry_ : program.functions[fn].name;
        }
        auto t = scheduler.spawn(fn);
        if (!t) {
            return failed(t.error());
        }
        std::lock_guard lock(mutex_);
        if (t->kind == vm::TaskOutcome::Kind::Done) {
            endThread(sb, key(fn, *t));
            if (i == 0) {
                result = t->value;
            }
        } else {
            threadOf(sb, t->task);
            if (i == 0) {
                entryTask = t->task;
            }
        }
        spawning_.clear();
    }
    while (!terminate_ && scheduler.nextTask()) {
        std::uint32_t task = *scheduler.nextTask();
        auto t = scheduler.next();
        if (!*t) {
            return failed(t->error());
        }
        if ((*t)->kind == vm::TaskOutcome::Kind::Done) {
            std::lock_guard lock(mutex_);
            endThread(sb, task);
            if (entryTask == task) {
                result = (*t)->value;
            }
        }
    }
    if (terminate_) {
        return 0;
    }
    if (result && program.functions[functions[0]].result == ir::kI64) {
        output("stdout", std::format("{}\n", result->i));
    }
    return 0;
}

} // namespace quartz::dap
