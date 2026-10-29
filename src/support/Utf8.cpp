#include "support/Utf8.hpp"

namespace quartz {

// One U+FFFD per maximal subpart of an ill-formed sequence (Unicode 3.9, WHATWG), as JavaScript, Python and Rust do.
std::string toValidUtf8(std::string_view text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    std::string out;
    out.reserve(n + 8);
    std::size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
            continue;
        }
        std::size_t len = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        unsigned char lo = c == 0xE0 ? 0xA0 : c == 0xF0 ? 0x90 : 0x80;
        unsigned char hi = c == 0xED ? 0x9F : c == 0xF4 ? 0x8F : 0xBF;
        std::size_t good = 1;
        while (len != 0 && good < len && i + good < n) {
            unsigned char b = p[i + good];
            if (good == 1 ? (b < lo || b > hi) : (b & 0xC0) != 0x80) {
                break;
            }
            ++good;
        }
        if (len != 0 && good == len) {
            out.append(text.substr(i, len));
        } else {
            out += "\xEF\xBF\xBD";
        }
        i += good;
    }
    return out;
}

} // namespace quartz
