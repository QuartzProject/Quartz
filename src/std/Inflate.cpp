#include "std/Deflate.hpp"

#include <array>

namespace quartz::stdlib {

namespace {

constexpr std::array<std::uint16_t, 29> kLenBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLenExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistBase = {1,   2,   3,   4,    5,    7,    9,    13,    17,    25,
                                                     33,  49,  65,  97,   129,  193,  257,  385,   513,   769,
                                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct Huffman {
    std::array<std::uint16_t, 16> count{};
    std::array<std::uint16_t, 320> symbol{};

    // Canonical code from lengths; false when the lengths over-subscribe the code space.
    bool build(const std::uint8_t* lengths, std::size_t n) {
        count.fill(0);
        for (std::size_t i = 0; i < n; ++i) {
            ++count[lengths[i]];
        }
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) {
                return false;
            }
        }
        std::array<std::uint16_t, 16> offs{};
        for (int len = 1; len < 15; ++len) {
            offs[len + 1] = static_cast<std::uint16_t>(offs[len] + count[len]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (lengths[i] != 0) {
                symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
            }
        }
        return true;
    }
};

class Reader {
public:
    Reader(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t maxOut)
        : in_(in), out_(out), maxOut_(maxOut) {}

    bool bits(int n, std::uint32_t& v) {
        while (count_ < n) {
            if (pos_ >= in_.size()) {
                return false;
            }
            buf_ |= static_cast<std::uint64_t>(in_[pos_++]) << count_;
            count_ += 8;
        }
        v = static_cast<std::uint32_t>(buf_ & ((1ULL << n) - 1));
        buf_ >>= n;
        count_ -= n;
        return true;
    }

    bool decode(const Huffman& h, int& sym) {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len < 16; ++len) {
            std::uint32_t b = 0;
            if (!bits(1, b)) {
                return false;
            }
            code |= static_cast<int>(b);
            int c = h.count[len];
            if (code - c < first) {
                sym = h.symbol[index + (code - first)];
                return true;
            }
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }

    bool stored(std::string& error) {
        buf_ = 0;
        count_ = 0;
        if (pos_ + 4 > in_.size()) {
            return fail(error, "truncated stored block");
        }
        std::uint32_t len = in_[pos_] | (in_[pos_ + 1] << 8);
        std::uint32_t nlen = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;
        if ((len ^ 0xFFFF) != nlen) {
            return fail(error, "stored block length mismatch");
        }
        if (pos_ + len > in_.size()) {
            return fail(error, "truncated stored block");
        }
        if (out_.size() + len > maxOut_) {
            return fail(error, "output too large");
        }
        out_.insert(out_.end(), in_.begin() + static_cast<std::ptrdiff_t>(pos_),
                    in_.begin() + static_cast<std::ptrdiff_t>(pos_ + len));
        pos_ += len;
        return true;
    }

    bool codes(const Huffman& lit, const Huffman& dist, std::string& error) {
        while (true) {
            int sym = 0;
            if (!decode(lit, sym)) {
                return fail(error, "invalid literal/length code");
            }
            if (sym < 256) {
                if (out_.size() >= maxOut_) {
                    return fail(error, "output too large");
                }
                out_.push_back(static_cast<std::uint8_t>(sym));
                continue;
            }
            if (sym == 256) {
                return true;
            }
            sym -= 257;
            if (sym >= 29) {
                return fail(error, "invalid length symbol");
            }
            std::uint32_t extra = 0;
            if (!bits(kLenExtra[sym], extra)) {
                return fail(error, "truncated input");
            }
            std::size_t len = kLenBase[sym] + extra;
            int dsym = 0;
            if (!decode(dist, dsym) || dsym >= 30) {
                return fail(error, "invalid distance code");
            }
            if (!bits(kDistExtra[dsym], extra)) {
                return fail(error, "truncated input");
            }
            std::size_t d = kDistBase[dsym] + extra;
            if (d > out_.size()) {
                return fail(error, "distance too far back");
            }
            if (out_.size() + len > maxOut_) {
                return fail(error, "output too large");
            }
            std::size_t from = out_.size() - d;
            for (std::size_t i = 0; i < len; ++i) {
                out_.push_back(out_[from + i]);
            }
        }
    }

    bool dynamic(std::string& error) {
        std::uint32_t nlen = 0;
        std::uint32_t ndist = 0;
        std::uint32_t ncode = 0;
        if (!bits(5, nlen) || !bits(5, ndist) || !bits(4, ncode)) {
            return fail(error, "truncated input");
        }
        nlen += 257;
        ndist += 1;
        ncode += 4;
        if (nlen > 286 || ndist > 30) {
            return fail(error, "bad code counts");
        }
        static constexpr std::array<std::uint8_t, 19> kOrder = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                                11, 4,  12, 3, 13, 2, 14, 1, 15};
        std::array<std::uint8_t, 320> lengths{};
        for (std::uint32_t i = 0; i < ncode; ++i) {
            std::uint32_t v = 0;
            if (!bits(3, v)) {
                return fail(error, "truncated input");
            }
            lengths[kOrder[i]] = static_cast<std::uint8_t>(v);
        }
        Huffman lencode;
        if (!lencode.build(lengths.data(), 19)) {
            return fail(error, "bad code lengths");
        }
        std::uint32_t index = 0;
        lengths.fill(0);
        while (index < nlen + ndist) {
            int sym = 0;
            if (!decode(lencode, sym)) {
                return fail(error, "bad code lengths");
            }
            if (sym < 16) {
                lengths[index++] = static_cast<std::uint8_t>(sym);
                continue;
            }
            std::uint8_t len = 0;
            std::uint32_t rep = 0;
            if (sym == 16) {
                if (index == 0) {
                    return fail(error, "repeat with no previous length");
                }
                len = lengths[index - 1];
                if (!bits(2, rep)) {
                    return fail(error, "truncated input");
                }
                rep += 3;
            } else if (sym == 17) {
                if (!bits(3, rep)) {
                    return fail(error, "truncated input");
                }
                rep += 3;
            } else {
                if (!bits(7, rep)) {
                    return fail(error, "truncated input");
                }
                rep += 11;
            }
            if (index + rep > nlen + ndist) {
                return fail(error, "too many code lengths");
            }
            while (rep-- > 0) {
                lengths[index++] = len;
            }
        }
        if (lengths[256] == 0) {
            return fail(error, "no end of block code");
        }
        Huffman lit;
        Huffman dist;
        if (!lit.build(lengths.data(), nlen) || !dist.build(lengths.data() + nlen, ndist)) {
            return fail(error, "bad literal or distance lengths");
        }
        return codes(lit, dist, error);
    }

    bool fixed(std::string& error) {
        static Huffman lit;
        static Huffman dist;
        static bool ready = [] {
            std::array<std::uint8_t, 320> l{};
            for (int i = 0; i < 144; ++i) {
                l[i] = 8;
            }
            for (int i = 144; i < 256; ++i) {
                l[i] = 9;
            }
            for (int i = 256; i < 280; ++i) {
                l[i] = 7;
            }
            for (int i = 280; i < 288; ++i) {
                l[i] = 8;
            }
            lit.build(l.data(), 288);
            std::array<std::uint8_t, 30> d{};
            d.fill(5);
            dist.build(d.data(), 30);
            return true;
        }();
        (void)ready;
        return codes(lit, dist, error);
    }

    bool run(std::string& error) {
        while (true) {
            std::uint32_t last = 0;
            std::uint32_t type = 0;
            if (!bits(1, last) || !bits(2, type)) {
                return fail(error, "truncated input");
            }
            bool ok = type == 0 ? stored(error) : type == 1 ? fixed(error) : type == 2 ? dynamic(error)
                                                                                     : fail(error, "invalid block type");
            if (!ok) {
                return false;
            }
            if (last != 0) {
                return true;
            }
        }
    }

    [[nodiscard]] std::size_t consumed() const { return pos_ - static_cast<std::size_t>(count_ / 8); }

private:
    static bool fail(std::string& error, const char* what) {
        if (error.empty()) {
            error = what;
        }
        return false;
    }

    std::span<const std::uint8_t> in_;
    std::vector<std::uint8_t>& out_;
    std::size_t maxOut_;
    std::size_t pos_ = 0;
    std::uint64_t buf_ = 0;
    int count_ = 0;
};

} // namespace

bool inflate(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t maxOut, std::string& error,
             std::size_t* consumed) {
    Reader r(in, out, maxOut);
    bool ok = r.run(error);
    if (consumed != nullptr) {
        *consumed = r.consumed();
    }
    return ok;
}

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) {
    static const std::array<std::uint32_t, 256> kTable = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = crc ^ 0xFFFFFFFFU;
    for (std::uint8_t b : data) {
        c = kTable[(c ^ b) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFU;
}

std::uint32_t adler32(std::span<const std::uint8_t> data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t x : data) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

} // namespace quartz::stdlib
