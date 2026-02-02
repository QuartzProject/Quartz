#pragma once

#include "Workspace.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::lsp {

struct BuiltinMethod {
    std::string name;
    std::string signature; // `name(params) -> result` with the receiver's types filled in
};

struct BuiltinFunction {
    std::string_view name;
    std::string_view signature;
};

/// Methods the compiler provides on `recv` (strings, lists, maps, numbers, nullables, results); mirrors sema.
std::vector<BuiltinMethod> builtinMethods(const Analysis& a, std::uint32_t recv);
std::optional<BuiltinMethod> findBuiltinMethod(const Analysis& a, std::uint32_t recv, std::string_view name);

std::span<const BuiltinFunction> builtinFunctions();
const BuiltinFunction* findBuiltinFunction(std::string_view name);

std::span<const std::string_view> builtinTypes();
std::span<const std::string_view> keywords();

} // namespace quartz::lsp
