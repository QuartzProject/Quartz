#pragma once

#include "ir/Ir.hpp"

#include <cstdint>
#include <vector>

namespace quartz::ir {

/// Where a suspended frame continues (D56): after a `wait` or a suspend call, before a call whose fuel charge
/// yielded, or at the head of a loop whose back edge yielded.
enum class ResumeAt : std::uint32_t { After = 0, BeforeCall = 1, LoopHead = 2 };

/// Identifies a suspension point of a function; the native tier and task migration share it.
inline std::uint32_t resumeState(std::size_t pc, ResumeAt at) {
    return static_cast<std::uint32_t>(pc << 2) | static_cast<std::uint32_t>(at);
}

/// Backward dataflow over the structured control flow: which registers a later instruction may still read.
class Liveness {
public:
    explicit Liveness(const Function& fn);

    /// Registers a frame suspended at `pc` must keep, ascending; the instruction's own result is excluded.
    [[nodiscard]] std::vector<Reg> kept(std::size_t pc, ResumeAt at) const;

private:
    [[nodiscard]] std::vector<Reg> set(const std::vector<std::uint64_t>& bits, std::size_t pc) const;

    const Function& fn_;
    std::size_t words_ = 0;
    std::vector<std::uint64_t> in_;
    std::vector<std::uint64_t> out_;
};

} // namespace quartz::ir
