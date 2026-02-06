#pragma once

#include "vm/Layout.hpp"
#include "vm/Value.hpp"

#include <cstddef>
#include <cstdint>

namespace quartz::vm {

/// C layout of a `@repr(C)` struct: natural alignment, fields in order, size rounded to the alignment.
struct CLayout {
    std::size_t size = 0;
    std::size_t align = 1;
};

CLayout cLayout(const ir::TypeTable& types, ir::TypeId t);
void toC(const ir::TypeTable& types, const Layouts& lay, ir::TypeId t, const Slot* src, std::uint8_t* dst);
void fromC(const ir::TypeTable& types, const Layouts& lay, ir::TypeId t, const std::uint8_t* src, Slot* dst);

} // namespace quartz::vm
