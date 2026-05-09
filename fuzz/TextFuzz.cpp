#include "StdFuzz.hpp"

namespace {

constexpr const char* kProgram = R"(import org.quartzlang.data.Value
import org.quartzlang.decimal.parseDecimal
import org.quartzlang.encoding.parseUuid
import org.quartzlang.http.parseUrl
import org.quartzlang.semver.parseSemver
import org.quartzlang.text.*
import org.quartzlang.time.*
fn run(text: str) -> int {
    var n = 0
    match parseUuid(text) {
        Ok(u) => assert(parseUuid(u.toStr()).unwrap().equals(u), "uuid round trip")
        Err(e) => n += e.len()
    }
    match parseDuration(text) {
        Ok(d) => assert(parseDuration(d.toStr()).unwrap().equals(d), "duration round trip")
        Err(e) => n += e.len()
    }
    match parseInstant(text) {
        Ok(t) => assert(parseInstant(t.toStr()).unwrap().equals(t), "instant round trip")
        Err(e) => n += e.len()
    }
    match parseCron(text) {
        Ok(c) => {
            if let next = c.next(fromEpochSeconds(1763563200)) {
                assert(c.matches(next), "cron next matches")
            }
        }
        Err(e) => n += e.len()
    }
    let args: Map<str, Value> = Map()
    args["n"] = Value.Int(text.len())
    args["s"] = Value.Str(text)
    for locale in ["en", "ru", "ar"] {
        match formatMessage(text, args, locale) {
            Ok(s) => n += s.len()
            Err(e) => n += e.len()
        }
    }
    let half = text.len() / 2
    if text.len() < 200 {
        let pattern = text.slice(0, half).toBytes().toStr() ?? ""
        let subject = text.slice(half, text.len()).toBytes().toStr() ?? ""
        n += if globMatch(pattern, subject) { 1 } else { 0 }
        n += if globMatch(pattern, subject, "/") { 1 } else { 0 }
        n += levenshtein(pattern, subject)
    }
    match parseUrl(text) {
        Ok(u) => assert(parseUrl(u.toStr()).unwrap().toStr() == u.toStr(), "url round trip")
        Err(e) => n += e.len()
    }
    match parseDecimal(text) {
        Ok(d) => assert(parseDecimal(d.toStr()).unwrap().equals(d), "decimal round trip")
        Err(e) => n += e.len()
    }
    match parseSemver(text) {
        Ok(v) => n += if v.satisfies(text).unwrapOr(false) { 1 } else { 0 }
        Err(e) => n += e.len()
    }
    return n
}
)";

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    return quartz::fuzz::runStd(kProgram, data, size, true);
}
