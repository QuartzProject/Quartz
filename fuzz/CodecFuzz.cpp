#include "StdFuzz.hpp"

namespace {

// Whatever decodes must survive encoding back unchanged (TOML moves tables after plain keys, so that one compares
// sorted), and whatever JSON parses must come back from the TOML writer as the same tree.
constexpr const char* kProgram = R"(import org.quartzlang.data.*
@derive(Decode, Encode)
enum Kind {
    Plain
    Pair(int, str)
    Nested([Item])
}
@derive(Decode, Encode)
struct Item {
    name: str = "x"
    @key("odd key")
    count: int?
    kind: Kind = Kind.Plain
    tags: Map<str, float> = Map()
    kids: [Item] = []
    raw: Value?
    small: i8 = 0
    letter: char = 'a'
}
fn check(r: Result<Item, DecodeError>) -> int {
    return match r {
        Ok(item) => {
            let json = encodeJson(item)
            assert(encodeJson(decodeJson::<Item>(json).unwrap()) == json, "json round trip")
            match encodeToml(item) {
                Ok(t) => assert(toJson(encode(decodeToml::<Item>(t).unwrap())) == toJson(encode(item)), "toml round trip")
                Err(e) => assert(e.contains("null"), "toml refused {e}")
            }
            1
        }
        Err(e) => e.message().len() + e.line
    }
}
fn run(text: str) -> int {
    var n = check(decodeToml::<Item>(text)) + check(decodeJson::<Item>(text)) + check(decodeYaml::<Item>(text))
    match parseJson(text) {
        Ok(v) => {
            match toToml(v) {
                Ok(t) => {
                    assert(toJson(parseToml(t).unwrap()) == toJson(v), "toml writer")
                }
                Err(e) => {
                    n += e.len()
                }
            }
        }
        Err(e) => {
            n += e.len()
        }
    }
    return n
}
)";

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return quartz::fuzz::runStd(kProgram, data, size, true);
}
