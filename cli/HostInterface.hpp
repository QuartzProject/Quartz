#pragma once

#include <string_view>

namespace quartz::cli {

/// Host interface the `quartz` tool provides to every program it checks or runs.
inline constexpr std::string_view kHostInterfaceName = "<std>.qzi";
inline constexpr std::string_view kHostInterface = R"(fn print(text: str)
@handle
@awaitable
struct Delay {}
impl int {
    @property
    fn ticks(self) -> Delay
}
)";

} // namespace quartz::cli
