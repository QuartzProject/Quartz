#pragma once

#include "vm/Sandbox.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

namespace quartz::dap {

/// The `quartz` tool's virtual tick clock: `wait n.ticks` resumes n ticks later, a fuel yield on the next tick.
class TickScheduler {
public:
    struct Pending {
        std::uint32_t task = 0;
        std::uint64_t wake = 0;
        std::uint64_t order = 0;
    };

    explicit TickScheduler(vm::Sandbox& sandbox) : sandbox_(sandbox) {}

    /// Starts `function` as a task (a plain function runs to its end) and queues it if it suspends.
    std::expected<vm::TaskOutcome, vm::Trap> spawn(std::uint32_t function);
    /// Resumes the task due first and moves the clock to its tick; nothing when no task is left.
    std::optional<std::expected<vm::TaskOutcome, vm::Trap>> next();
    /// Cancels every queued task.
    void cancelAll();

    [[nodiscard]] const std::vector<Pending>& pending() const { return pending_; }

    [[nodiscard]] std::uint64_t now() const { return now_; }

    [[nodiscard]] std::uint64_t ticksWaited() const { return waited_; }

    [[nodiscard]] std::uint64_t yields() const { return yields_; }

private:
    void queue(const std::expected<vm::TaskOutcome, vm::Trap>& outcome);

    vm::Sandbox& sandbox_;
    std::vector<Pending> pending_; // sorted by wake tick, then order
    std::uint64_t now_ = 0;
    std::uint64_t order_ = 0;
    std::uint64_t waited_ = 0;
    std::uint64_t yields_ = 0;
};

} // namespace quartz::dap
