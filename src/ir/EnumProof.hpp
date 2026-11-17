#pragma once

#include "ir/Ir.hpp"

#include <vector>

namespace quartz::ir {

/// Per instruction: an `enum.field` whose variant a tag test earlier on every path to it already proved (D231).
/// Both tiers skip the tag check there; the proof reads only the validated IR, never trusts the frontend.
std::vector<bool> provenEnumFields(const Module& m, const Function& fn);

} // namespace quartz::ir
