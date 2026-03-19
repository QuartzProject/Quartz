#include "StdFuzz.hpp"

namespace {

constexpr const char* kProgram = R"(import org.quartzlang.data.*
fn run(text: str) -> int {
    var n = 0
    match parseJson(text) {
        Ok(v) => n += toJson(v).len() + toJson(v, 2).len()
        Err(e) => n += e.len()
    }
    match parseToml(text) {
        Ok(v) => n += toJson(v).len()
        Err(e) => n += e.len()
    }
    match parseYaml(text) {
        Ok(v) => n += toJson(v).len()
        Err(e) => n += e.len()
    }
    return n
}
)";

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return quartz::fuzz::runStd(kProgram, data, size);
}
