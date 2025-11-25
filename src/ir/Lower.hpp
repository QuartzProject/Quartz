#pragma once

#include "ir/Ir.hpp"
#include "sema/Sema.hpp"

#include <memory>

namespace quartz::ir {

/// Lowers a checked program to Quartz IR. Only call it when sema reported no errors.
std::unique_ptr<Module> lower(sema::Program& program, const SourceManager& sources, Diagnostics& diags);

} // namespace quartz::ir
