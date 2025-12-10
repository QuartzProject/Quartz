#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace quartz {
class Interner;

namespace syntax {
struct Module;
}
} // namespace quartz

namespace quartz::format {

/// Formats one source file; fails with the rendered diagnostics when it does not parse or would change meaning.
std::expected<std::string, std::string> formatSource(std::string_view name, std::string_view text);

/// Span-free dump of a module: two sources with the same dump parse to the same tree.
std::string dumpModule(const syntax::Module& m, const Interner& names);

/// Parses `text` and returns its dump, or the rendered diagnostics.
std::expected<std::string, std::string> dumpSource(std::string_view name, std::string_view text);

} // namespace quartz::format
