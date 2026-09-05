#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

namespace quartz {

/// True when `text` is well-formed UTF-8: no overlong forms, surrogates, code points above U+10FFFF or cut sequences.
inline bool validUtf8(std::string_view text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    std::size_t i = 0;
    constexpr std::uint64_t kHigh = 0x8080808080808080ULL;
    while (i + 32 <= n) {
        std::uint64_t w[4];
        std::memcpy(w, p + i, 32);
        if (((w[0] | w[1] | w[2] | w[3]) & kHigh) != 0) {
            break;
        }
        i += 32;
    }
    while (i < n) {
        if (i + 8 <= n) {
            std::uint64_t word = 0;
            std::memcpy(&word, p + i, 8);
            if ((word & kHigh) == 0) {
                i += 8;
                continue;
            }
        }
        unsigned char c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t len = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        if (len == 0 || len > n - i) {
            return false;
        }
        unsigned char c1 = p[i + 1];
        if ((c1 & 0xC0) != 0x80 || (c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F) || (c == 0xF0 && c1 < 0x90) ||
            (c == 0xF4 && c1 > 0x8F)) {
            return false;
        }
        for (std::size_t k = 2; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

} // namespace quartz
