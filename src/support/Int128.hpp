#pragma once

#include <cstdint>

namespace quartz::support {

using i128 = __int128;
using u128 = unsigned __int128;

// Not `/` and `%` on __int128: those need __divti3 and friends from compiler-rt, which not every target links.
inline u128 divmodUnsigned(u128 n, u128 d, u128& remainder) {
    if ((n >> 64) == 0 && (d >> 64) == 0) {
        const auto a = static_cast<std::uint64_t>(n);
        const auto b = static_cast<std::uint64_t>(d);
        remainder = a % b;
        return a / b;
    }
    if (n < d) {
        remainder = n;
        return 0;
    }
    if ((d >> 127) != 0) {
        remainder = n - d;
        return 1;
    }
    u128 quotient = 0;
    u128 rest = 0;
    for (int bit = 127; bit >= 0; --bit) {
        rest = (rest << 1) | ((n >> bit) & 1);
        if (rest >= d) {
            rest -= d;
            quotient |= u128{1} << bit;
        }
    }
    remainder = rest;
    return quotient;
}

// Truncates toward zero like C++ division; the remainder takes the sign of the dividend. `d` must not be zero.
inline i128 divmod(i128 n, i128 d, i128& remainder) {
    const bool negativeN = n < 0;
    const bool negativeD = d < 0;
    const u128 un = negativeN ? u128{0} - static_cast<u128>(n) : static_cast<u128>(n);
    const u128 ud = negativeD ? u128{0} - static_cast<u128>(d) : static_cast<u128>(d);
    u128 rest = 0;
    const u128 quotient = divmodUnsigned(un, ud, rest);
    remainder = static_cast<i128>(negativeN ? u128{0} - rest : rest);
    return static_cast<i128>(negativeN != negativeD ? u128{0} - quotient : quotient);
}

} // namespace quartz::support
