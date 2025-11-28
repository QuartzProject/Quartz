#pragma once

#include "ir/Ir.hpp"

namespace quartz::ir {

/// Semantics-preserving IR cleanups; the result is validated like any other module.
void optimize(Module& m);

} // namespace quartz::ir
