#pragma once

#include "ir/Ir.hpp"

#include <functional>
#include <string_view>

namespace quartz::ir {

/// Drops the library functions (source file starting with `libraryPrefix`) and library imports that nothing else
/// reaches, and unexports the library functions that stay: the host only sees the program's own functions.
void pruneLibrary(Module& m, std::string_view libraryPrefix,
                  const std::function<bool(std::string_view)>& isLibraryImport);

} // namespace quartz::ir
