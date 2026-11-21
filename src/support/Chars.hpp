#pragma once

#include <charconv>

namespace quartz::support {

/// std::from_chars for a double in the general format, also where libc++ lacks it (before 20, or macOS before 26).
std::from_chars_result parseDouble(const char* first, const char* last, double& value);

} // namespace quartz::support
