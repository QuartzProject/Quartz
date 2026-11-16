#pragma once

#include "vm/Sandbox.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

namespace quartz::vm {

/// A virtual tick clock for hosts and tools: `wait n.ticks` resumes n ticks later, a fuel yield on the next tick.
class TickScheduler {
public:
    struct Pending {
        std::uint32_t task = 0;
        std::uint64_t wake = 0;
        std::uint64_t order = 0;
    };

    explicit TickScheduler(Sandbox& sandbox) : sandbox_(sandbox) {}

    /// Starts `function` as a task (a plain function runs to its end) and queues it if it suspends.
    std::expected<TaskOutcome, Trap> spawn(std::uint32_t function);
    /// Resumes the task due first and moves the clock to its tick; nothing when no task is left.
    std::optional<std::expected<TaskOutcome, Trap>> next();
    /// Cancels every queued task.
    void cancelAll();

    [[nodiscard]] const std::vector<Pending>& pending() const { return pending_; }

    /// The tick the next `next()` moves the clock to; nothing when no task is queued.
    [[nodiscard]] std::optional<std::uint64_t> nextWake() const {
        return pending_.empty() ? std::nullopt : std::optional(pending_.front().wake);
    }

    /// The task the next `next()` resumes; nothing when no task is queued.
    [[nodiscard]] std::optional<std::uint32_t> nextTask() const {
        return pending_.empty() ? std::nullopt : std::optional(pending_.front().task);
    }

    [[nodiscard]] std::uint64_t now() const { return now_; }

    [[nodiscard]] std::uint64_t ticksWaited() const { return waited_; }

    [[nodiscard]] std::uint64_t yields() const { return yields_; }

private:
    void queue(const std::expected<TaskOutcome, Trap>& outcome);

    Sandbox& sandbox_;
    std::vector<Pending> pending_; // sorted by wake tick, then order
    std::uint64_t now_ = 0;
    std::uint64_t order_ = 0;
    std::uint64_t waited_ = 0;
    std::uint64_t yields_ = 0;
};

} // namespace quartz::vm
