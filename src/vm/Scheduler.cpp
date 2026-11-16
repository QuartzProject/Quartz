#include "vm/Scheduler.hpp"

#include <algorithm>

namespace quartz::vm {

std::expected<TaskOutcome, Trap> TickScheduler::spawn(std::uint32_t function) {
    auto t = sandbox_.spawn(function, {});
    queue(t);
    return t;
}

std::optional<std::expected<TaskOutcome, Trap>> TickScheduler::next() {
    if (pending_.empty()) {
        return std::nullopt;
    }
    Pending p = pending_.front();
    pending_.erase(pending_.begin());
    now_ = std::max(now_, p.wake);
    auto t = sandbox_.resume(p.task);
    queue(t);
    return t;
}

void TickScheduler::cancelAll() {
    for (const Pending& p : pending_) {
        sandbox_.cancel(p.task);
    }
    pending_.clear();
}

void TickScheduler::queue(const std::expected<TaskOutcome, Trap>& outcome) {
    if (!outcome || outcome->kind == TaskOutcome::Kind::Done) {
        return;
    }
    std::uint64_t wake = now_;
    if (outcome->kind == TaskOutcome::Kind::Wait) {
        waited_ += outcome->value.u;
        wake = outcome->value.u > ~wake ? ~std::uint64_t{0} : wake + outcome->value.u;
    } else if (outcome->kind == TaskOutcome::Kind::Yield) {
        ++yields_;
        wake += 1;
    }
    Pending p{outcome->task, wake, order_++};
    auto at = std::upper_bound(pending_.begin(), pending_.end(), p, [](const Pending& a, const Pending& b) {
        return a.wake != b.wake ? a.wake < b.wake : a.order < b.order;
    });
    pending_.insert(at, p);
}

} // namespace quartz::vm
