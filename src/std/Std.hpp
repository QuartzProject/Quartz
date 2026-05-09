#pragma once

#include "vm/Sandbox.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace quartz::stdlib {

/// Embedded sources of the standard library: (file name, text). Compiled with every program.
const std::vector<std::pair<std::string_view, std::string_view>>& sources();
/// The first sources, compiled with every program; the others only when their package is imported.
std::size_t coreSourceCount();
/// The package a source declares on its first line.
std::string_view packageOf(std::string_view source);

struct Native {
    std::string_view name;
    vm::HostFn fn;
};

/// What a native receives as its user pointer: its own import (for result types) and the data types.
struct NativeCtx {
    const ir::HostImport* import = nullptr;
    const DataTypes* data = nullptr;
};

/// Parses JSON into a `org.quartzlang.data.Value` in the running sandbox's arena; false with `error` set on bad input.
bool jsonToValue(vm::Sandbox& sb, const DataTypes& types, std::string_view text, vm::Slot* out, std::string& error);

/// Native implementations of the `org.quartzlang.*` interface functions.
const std::vector<Native>& natives();
/// True for the name of a native import of the standard library.
bool isNative(std::string_view name);

/// Type ids the data natives need, found from the program's own `org.quartzlang.data` declarations.
struct DataTypes {
    ir::TypeId value = 0;
    ir::TypeId list = 0;
    ir::TypeId table = 0;
    std::uint32_t valueSlots = 2;
};

} // namespace quartz::stdlib
