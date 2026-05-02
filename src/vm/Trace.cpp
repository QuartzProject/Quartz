#include "vm/Sandbox.hpp"

namespace quartz::vm {

namespace {

// A trace is for people: deep recursion keeps the innermost calls and the outermost one.
constexpr std::size_t kMaxCallers = 64;

} // namespace

void Sandbox::addCaller(std::uint32_t function, std::uint32_t line) {
    if (function < program_->functions.size() && trap_.callers.size() < kMaxCallers) {
        trap_.callers.emplace_back(program_->functions[function].name, line);
    }
}

void Sandbox::traceCallers(std::size_t entryDepth) {
    for (std::size_t i = calls_.size(); i > entryDepth && trap_.callers.size() < kMaxCallers; --i) {
        const CallRecord& r = calls_[i - 1];
        auto at = static_cast<std::size_t>(r.pc - r.fn->code.data());
        std::uint32_t line = at > 0 && at - 1 < r.fn->lines.size() ? r.fn->lines[at - 1] : 0;
        trap_.callers.emplace_back(r.fn->name, line);
    }
}

} // namespace quartz::vm
