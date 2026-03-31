#pragma once

#include "Workspace.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quartz::lsp {

enum class ExportKind : std::uint8_t { Function, Struct, Enum, Trait, Const };

/// A public top-level symbol of a named package, importable from other packages.
struct Export {
    std::string package;
    std::string name;
    ExportKind kind;
    std::string detail;
};

/// Every exported symbol of the program, the math built-ins of org.quartzlang.math included.
std::vector<Export> exports(const Analysis& a);

/// What a module's `package` and `import` lines make visible.
struct ImportScope {
    std::string package;
    std::set<std::string> star;
    std::map<std::string, std::pair<std::string, std::string>> names; // local name -> (package, name)
    std::map<std::string, std::string> qualifiers;                    // `import a.b.c` makes `c` name a.b.c

    /// Whether `name` of `package` is reachable without a qualifier, under its own name or an alias.
    [[nodiscard]] bool imports(std::string_view package, std::string_view name) const;
    /// The local name `import a.b.c` gives to a package, or empty.
    [[nodiscard]] std::string qualifierOf(std::string_view package) const;
};

ImportScope importScope(const Analysis& a, const syntax::Module& m);

std::string joinPath(const Analysis& a, std::span<const Symbol> path);

struct TextEdit {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::string text;
};

/// The edit that adds `import package.name` to a file, in sorted order among its imports.
TextEdit importEdit(const Analysis& a, std::uint32_t file, std::string_view package, std::string_view name);

} // namespace quartz::lsp
