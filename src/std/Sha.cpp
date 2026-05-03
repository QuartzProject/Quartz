#include "std/Sha.hpp"

#include <bit>
#include <cstring>

namespace quartz::stdlib {

namespace {

using u128 = unsigned __int128;

constexpr std::uint64_t rootFloor(u128 x, int degree) {
    std::uint64_t lo = 0;
    std::uint64_t hi = std::uint64_t{1} << (degree == 2 ? 63 : 42);
    while (lo < hi) {
        std::uint64_t mid = lo + (hi - lo + 1) / 2;
        u128 p = static_cast<u128>(mid) * mid;
        if (degree == 3) {
            p *= mid;
        }
        if (p <= x) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

constexpr std::array<std::uint32_t, 64> firstPrimes() {
    std::array<std::uint32_t, 64> out{};
    std::uint32_t n = 2;
    for (std::size_t found = 0; found < out.size(); ++n) {
        bool prime = true;
        for (std::uint32_t d = 2; d * d <= n; ++d) {
            prime = prime && n % d != 0;
        }
        if (prime) {
            out[found++] = n;
        }
    }
    return out;
}

constexpr auto kPrimes = firstPrimes();

// The first 32 bits of the fractional parts of the square and cube roots of the first primes (FIPS 180-4, 4.2.2).
constexpr std::array<std::uint32_t, 64> kK256 = [] {
    std::array<std::uint32_t, 64> k{};
    for (std::size_t i = 0; i < k.size(); ++i) {
        k[i] = static_cast<std::uint32_t>(rootFloor(static_cast<u128>(kPrimes[i]) << 96, 3));
    }
    return k;
}();

constexpr std::array<std::uint32_t, 8> kH256 = [] {
    std::array<std::uint32_t, 8> h{};
    for (std::size_t i = 0; i < h.size(); ++i) {
        h[i] = static_cast<std::uint32_t>(rootFloor(static_cast<u128>(kPrimes[i]) << 64, 2));
    }
    return h;
}();

// 2^30 times the square roots of 2, 3, 5 and 10 (FIPS 180-4, 4.2.1).
constexpr std::array<std::uint32_t, 4> kK1 = {
    static_cast<std::uint32_t>(rootFloor(static_cast<u128>(2) << 60, 2)),
    static_cast<std::uint32_t>(rootFloor(static_cast<u128>(3) << 60, 2)),
    static_cast<std::uint32_t>(rootFloor(static_cast<u128>(5) << 60, 2)),
    static_cast<std::uint32_t>(rootFloor(static_cast<u128>(10) << 60, 2)),
};

// The bytes 01 23 45 67 89 ab cd ef fe dc ba 98 76 54 32 10 f0 e1 d2 c3, read as little-endian words.
constexpr std::array<std::uint32_t, 5> kH1 = [] {
    std::array<std::uint8_t, 20> bytes{};
    for (std::uint8_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(i * 0x22 + 1);
        bytes[8 + i] = static_cast<std::uint8_t>(0xFE - i * 0x22);
    }
    for (std::uint8_t i = 0; i < 4; ++i) {
        bytes[16 + i] = static_cast<std::uint8_t>(0xF0 - i * 0x0F);
    }
    std::array<std::uint32_t, 5> h{};
    for (std::size_t w = 0; w < h.size(); ++w) {
        for (int b = 3; b >= 0; --b) {
            h[w] = (h[w] << 8) | bytes[w * 4 + static_cast<std::size_t>(b)];
        }
    }
    return h;
}();

std::uint32_t loadBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

void storeBe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

template <class Hash>
void feed(Hash& h, std::array<std::uint8_t, 64>& buf, std::uint64_t& total, std::span<const std::uint8_t> data) {
    std::size_t used = total % 64;
    total += data.size();
    if (used > 0) {
        std::size_t take = std::min(64 - used, data.size());
        std::memcpy(buf.data() + used, data.data(), take);
        data = data.subspan(take);
        if (used + take < 64) {
            return;
        }
        h.block(buf.data());
    }
    while (data.size() >= 64) {
        h.block(data.data());
        data = data.subspan(64);
    }
    if (!data.empty()) {
        std::memcpy(buf.data(), data.data(), data.size());
    }
}

template <class Hash>
void pad(Hash& h, std::uint64_t total) {
    std::array<std::uint8_t, 72> tail{};
    tail[0] = 0x80;
    std::size_t padLen = (total % 64 < 56 ? 56 : 120) - total % 64;
    std::uint64_t bits = total * 8;
    for (int i = 0; i < 8; ++i) {
        tail[padLen + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    }
    h.update(std::span<const std::uint8_t>(tail.data(), padLen + 8));
}

} // namespace

Sha256::Sha256() : h_(kH256) {}

void Sha256::block(const std::uint8_t* p) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t t = 0; t < 16; ++t) {
        w[t] = loadBe32(p + 4 * t);
    }
    for (std::size_t t = 16; t < 64; ++t) {
        std::uint32_t s0 = std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        std::uint32_t s1 = std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = h_;
    for (std::size_t t = 0; t < 64; ++t) {
        std::uint32_t t1 =
            h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK256[t] + w[t];
        std::uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) { feed(*this, buf_, total_, data); }

std::array<std::uint8_t, 32> Sha256::final() {
    pad(*this, total_);
    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        storeBe32(out.data() + 4 * i, h_[i]);
    }
    return out;
}

Sha1::Sha1() : h_(kH1) {}

void Sha1::block(const std::uint8_t* p) {
    std::array<std::uint32_t, 80> w{};
    for (std::size_t t = 0; t < 16; ++t) {
        w[t] = loadBe32(p + 4 * t);
    }
    for (std::size_t t = 16; t < 80; ++t) {
        w[t] = std::rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    }
    auto [a, b, c, d, e] = h_;
    for (std::size_t t = 0; t < 80; ++t) {
        std::uint32_t f = t < 20 ? (b & c) ^ (~b & d) : t < 40 || t >= 60 ? b ^ c ^ d : (b & c) ^ (b & d) ^ (c & d);
        std::uint32_t temp = std::rotl(a, 5) + f + e + kK1[t / 20] + w[t];
        e = d;
        d = c;
        c = std::rotl(b, 30);
        b = a;
        a = temp;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(std::span<const std::uint8_t> data) { feed(*this, buf_, total_, data); }

std::array<std::uint8_t, 20> Sha1::final() {
    pad(*this, total_);
    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        storeBe32(out.data() + 4 * i, h_[i]);
    }
    return out;
}

std::array<std::uint8_t, 32> hmacSha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 64> k{};
    if (key.size() > k.size()) {
        Sha256 h;
        h.update(key);
        auto d = h.final();
        std::memcpy(k.data(), d.data(), d.size());
    } else if (!key.empty()) {
        std::memcpy(k.data(), key.data(), key.size());
    }
    std::array<std::uint8_t, 64> pad{};
    Sha256 inner;
    for (std::size_t i = 0; i < pad.size(); ++i) {
        pad[i] = k[i] ^ 0x36;
    }
    inner.update(pad);
    inner.update(data);
    auto innerHash = inner.final();
    Sha256 outer;
    for (std::size_t i = 0; i < pad.size(); ++i) {
        pad[i] = k[i] ^ 0x5c;
    }
    outer.update(pad);
    outer.update(innerHash);
    return outer.final();
}

} // namespace quartz::stdlib
