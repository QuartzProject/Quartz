#pragma once

#include "vm/Sandbox.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace quartz::stdlib {

/// Embedded sources of the standard library: (file name, text). Compiled with every program.
const std::vector<std::pair<std::string_view, std::string_view>>& sources();

struct Native {
    std::string_view name;
    vm::HostFn fn;
};

/// Native implementations of the `std.*` interface functions.
const std::vector<Native>& natives();

/// Type ids the data natives need, found from the program's own `std.data` declarations.
struct DataTypes {
    ir::TypeId value = 0;
    ir::TypeId list = 0;
    ir::TypeId table = 0;
    std::uint32_t valueSlots = 2;
};

} // namespace quartz::stdlib
