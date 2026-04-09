#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace quartz::syntax {

/// The `///` lines right above the line holding `offset`, attribute lines skipped, markers removed; empty when none.
std::string docComment(std::string_view text, std::uint32_t offset);

} // namespace quartz::syntax
