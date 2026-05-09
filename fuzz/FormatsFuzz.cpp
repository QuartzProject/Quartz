#include "StdFuzz.hpp"

namespace {

constexpr const char* kProgram = R"(import org.quartzlang.data.toJson
import org.quartzlang.encoding.*
import org.quartzlang.formats.*
import org.quartzlang.http.{decodeQuery, encodeQuery, percentDecode, percentEncode}
fn run(text: str) -> int {
    var n = 0
    match parseCsv(text) {
        Ok(rows) => assert(toCsv(parseCsv(toCsv(rows)).unwrap()) == toCsv(rows), "csv round trip")
        Err(e) => n += e.len()
    }
    match parseProperties(text) {
        Ok(m) => {
            let back = parseProperties(toProperties(m)).unwrap()
            assert(toProperties(back) == toProperties(m), "properties round trip")
        }
        Err(e) => n += e.len()
    }
    match parseMsgPack(text.toBytes()) {
        Ok(v) => assert(toJson(parseMsgPack(toMsgPack(v)).unwrap()) == toJson(v), "msgpack round trip")
        Err(e) => n += e.len()
    }
    for url in [false, true] {
        match base64Decode(text, url) {
            Ok(b) => assert(base64Decode(base64Encode(b, url, false), url).unwrap().hex() == b.hex(), "base64 round trip")
            Err(e) => n += e.len()
        }
    }
    match hexDecode(text) {
        Ok(b) => n += b.len()
        Err(e) => n += e.len()
    }
    match percentDecode(text) {
        Ok(s) => assert(percentDecode(percentEncode(s)).unwrap() == s, "percent round trip")
        Err(e) => n += e.len()
    }
    match decodeQuery(text) {
        Ok(q) => assert(encodeQuery(decodeQuery(encodeQuery(q)).unwrap()) == encodeQuery(q), "query round trip")
        Err(e) => n += e.len()
    }
    return n
}
)";

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return quartz::fuzz::runStd(kProgram, data, size, true);
}
