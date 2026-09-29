#pragma once

#include <string_view>

namespace quartz {
class SourceManager;
}

namespace quartz::sema {
struct Program;
}

namespace quartz::ir {
struct Module;
}

namespace quartz {

/// Keeps the `///` text and literal defaults of exported functions and the program's structs and enums in `m`.
void collectDocs(const sema::Program& prog, const SourceManager& sources, ir::Module& m);

/// True when `text` is a one-line literal, or a list or tuple of them, that a generated interface may repeat.
bool interfaceLiteral(std::string_view text);

} // namespace quartz
