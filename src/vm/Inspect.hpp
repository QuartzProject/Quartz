#pragma once

#include "vm/Bytecode.hpp"

#include <string>
#include <vector>

namespace quartz::vm {

/// A value in a sandbox's memory, read by debuggers while the sandbox is stopped: its type and its first slot.
struct ValueRef {
    ir::TypeId type = ir::kVoid;
    const Slot* at = nullptr;
    Slot copy{}; // packed `[bool]` elements have no slot of their own
    bool copied = false;

    [[nodiscard]] const Slot* slots() const { return copied ? &copy : at; }
};

struct ValueChild {
    std::string name;
    ValueRef value;
};

/// One line for a value: `42`, `"text"`, `Point { x: 1, y: 2 }`, `[1, 2, 3]`, `Some(4)`, `null`.
std::string showValue(const Program& p, const ValueRef& v, std::size_t budget = 120);

/// Fields, elements, map entries or enum payload, from `start` and at most `limit`; empty for scalars and text.
std::vector<ValueChild> childrenOf(const Program& p, const ValueRef& v, std::size_t start = 0,
                                   std::size_t limit = 1000);

/// How many children `childrenOf` can give.
std::size_t childCount(const Program& p, const ValueRef& v);

/// The value a nullable holds (or the value itself when it is not nullable); false when it is null.
bool unwrapNullable(const Program& p, ValueRef& v);

} // namespace quartz::vm
