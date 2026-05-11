#include "vm/Sandbox.hpp"

namespace quartz::vm {

namespace {

constexpr std::uint32_t kReplacement = 0xFFFD;

bool continuation(unsigned char b, unsigned char lo = 0x80, unsigned char hi = 0xBF) { return b >= lo && b <= hi; }

} // namespace

// Strict UTF-8 (Unicode table 3-7); a byte that does not start a well-formed sequence decodes to U+FFFD and only
// that byte is consumed, as Go's `for range` does.
bool Sandbox::decodeChar(const Str* s, std::int64_t at, Slot* dst) {
    if (at < 0 || at >= static_cast<std::int64_t>(s->len)) {
        return fail(ir::TrapCode::IndexOutOfBounds);
    }
    const auto* p = reinterpret_cast<const unsigned char*>(s->data) + at;
    std::size_t left = s->len - static_cast<std::size_t>(at);
    unsigned char b0 = p[0];
    std::uint32_t cp = kReplacement;
    std::uint32_t len = 1;
    if (b0 < 0x80) {
        cp = b0;
    } else if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (left >= 2 && continuation(p[1])) {
            cp = (b0 & 0x1Fu) << 6 | (p[1] & 0x3Fu);
            len = 2;
        }
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        unsigned char lo = b0 == 0xE0 ? 0xA0 : 0x80;
        unsigned char hi = b0 == 0xED ? 0x9F : 0xBF;
        if (left >= 3 && continuation(p[1], lo, hi) && continuation(p[2])) {
            cp = (b0 & 0x0Fu) << 12 | (p[1] & 0x3Fu) << 6 | (p[2] & 0x3Fu);
            len = 3;
        }
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        unsigned char lo = b0 == 0xF0 ? 0x90 : 0x80;
        unsigned char hi = b0 == 0xF4 ? 0x8F : 0xBF;
        if (left >= 4 && continuation(p[1], lo, hi) && continuation(p[2]) && continuation(p[3])) {
            cp = (b0 & 0x07u) << 18 | (p[1] & 0x3Fu) << 12 | (p[2] & 0x3Fu) << 6 | (p[3] & 0x3Fu);
            len = 4;
        }
    }
    dst[0].u = cp;
    dst[1].i = at + len;
    return true;
}

} // namespace quartz::vm
