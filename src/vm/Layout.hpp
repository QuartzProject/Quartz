#pragma once

#include "ir/Types.hpp"

#include <vector>

namespace quartz::vm {

/// How a value of each type is spread over 8-byte slots.
class Layouts {
public:
    explicit Layouts(const ir::TypeTable& types);

    [[nodiscard]] std::uint32_t slots(ir::TypeId t) const { return slots_[t]; }
    /// Offset in slots of field `i` of a struct, state or tuple.
    [[nodiscard]] std::uint32_t fieldOffset(ir::TypeId t, std::uint32_t i) const { return fieldOffsets_[t][i]; }
    /// Nullable values whose inner type is a reference use a null pointer; the rest carry a flag slot first.
    [[nodiscard]] bool nullableIsPointer(ir::TypeId t) const { return pointerNullable_[t]; }
    [[nodiscard]] const ir::TypeTable& types() const { return types_; }
    [[nodiscard]] std::uint32_t stateSlots(ir::TypeId t) const { return stateSlots_[t]; }

private:
    std::uint32_t compute(ir::TypeId t);

    const ir::TypeTable& types_;
    std::vector<std::uint32_t> slots_;
    std::vector<std::vector<std::uint32_t>> fieldOffsets_;
    std::vector<bool> pointerNullable_;
    std::vector<std::uint32_t> stateSlots_;
    std::vector<std::uint8_t> state_; // 0 unvisited, 1 visiting, 2 done
};

} // namespace quartz::vm
