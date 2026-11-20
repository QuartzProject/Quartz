#pragma once

#include <charconv>

namespace quartz::support {

/// std::from_chars for a double in the general format, also on a libc++ older than 20, which lacks it.
std::from_chars_result parseDouble(const char* first, const char* last, double& value);

} // namespace quartz::support
