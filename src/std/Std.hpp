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

/// What a native receives as its user pointer: its own import (for result types) and the data types.
struct NativeCtx {
    const ir::HostImport* import = nullptr;
    const DataTypes* data = nullptr;
};

/// Parses JSON into a `std.data.Value` in the running sandbox's arena; false with `error` set on bad input.
bool jsonToValue(vm::Sandbox& sb, const DataTypes& types, std::string_view text, vm::Slot* out, std::string& error);

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
