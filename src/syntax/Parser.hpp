#pragma once

#include "support/Arena.hpp"
#include "support/Diagnostics.hpp"
#include "support/Interner.hpp"
#include "syntax/Ast.hpp"
#include "syntax/Token.hpp"

#include <vector>

namespace quartz::syntax {

/// Parses one file. Never fails hard: errors go to `diags` and the module holds what could be recovered.
Module* parseModule(const SourceFile& file, Arena& arena, Interner& names, Diagnostics& diags);

} // namespace quartz::syntax
