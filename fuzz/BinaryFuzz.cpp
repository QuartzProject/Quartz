#include "StdFuzz.hpp"

namespace {

constexpr const char* kProgram = R"(import org.quartzlang.bytes.*
import org.quartzlang.compress.*
import org.quartzlang.zip.*
fn run(text: str) -> int {
    let data = text.toBytes()
    var n = 0
    match inflate(data) {
        Ok(v) => n += v.len()
        Err(e) => n += e.len()
    }
    match gunzip(data) {
        Ok(v) => n += v.len()
        Err(e) => n += e.len()
    }
    match zlibDecompress(data) {
        Ok(v) => n += v.len()
        Err(e) => n += e.len()
    }
    match zstdDecompress(data) {
        Ok(v) => n += v.len()
        Err(e) => n += e.len()
    }
    match readZip(data) {
        Ok(entries) => n += entries.len()
        Err(e) => n += e.len()
    }
    n += inflate(deflate(data, 6)).unwrap().len()
    n += zstdDecompress(zstdCompress(data, 3)).unwrap().len()
    return n
}
)";

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return quartz::fuzz::runStd(kProgram, data, size);
}
