#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::ir::bin {

/// Section ids of the payload, in the order they must appear.
enum class Section : std::uint8_t { Types = 1, Constants, Imports, States, Functions, Annotations, Lines, Manifest };

enum class TypeTag : std::uint8_t {
    List = 1,
    Map,
    Nullable,
    Tuple,
    Fn,
    Result,
    Struct,
    State,
    Enum,
    Handle,
    Param,
    Array
};

inline constexpr std::uint8_t kHasDst = 1;
inline constexpr std::uint8_t kHasA = 2;
inline constexpr std::uint8_t kHasB = 4;
inline constexpr std::uint8_t kHasImm = 8;
inline constexpr std::uint8_t kHasArgs = 16;
inline constexpr std::uint8_t kInstMask = 31;

inline constexpr std::uint8_t kFnExported = 1;
inline constexpr std::uint8_t kFnSuspend = 2;
inline constexpr std::uint8_t kFnInline = 4;
inline constexpr std::uint8_t kFnNoInline = 8;
inline constexpr std::uint8_t kFnMask = 15;

inline std::uint64_t zigzag(std::int64_t v) {
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}

inline std::int64_t unzigzag(std::uint64_t u) {
    return static_cast<std::int64_t>(u >> 1) ^ -static_cast<std::int64_t>(u & 1);
}

inline std::size_t ulebSize(std::uint64_t v) {
    std::size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

class Writer {
public:
    std::vector<std::uint8_t> out;

    void u8(std::uint8_t v) { out.push_back(v); }

    void u64(std::uint64_t v) {
        while (v >= 0x80) {
            out.push_back(static_cast<std::uint8_t>(v | 0x80));
            v >>= 7;
        }
        out.push_back(static_cast<std::uint8_t>(v));
    }

    void u32(std::uint32_t v) { u64(v); }

    void i64(std::int64_t v) { u64(zigzag(v)); }

    /// Indices where ~0 means none: written as index + 1 so none is a single zero byte.
    void opt(std::uint32_t v) { u64(v + 1); }

    void fixed64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }

    void str(std::string_view s) {
        u64(s.size());
        out.insert(out.end(), s.begin(), s.end());
    }

    void bytes(std::span<const std::uint8_t> b) { out.insert(out.end(), b.begin(), b.end()); }
};

/// Reads canonical encodings only; any malformed or out of range value sets a sticky failure and yields zero.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> in) : in_(in) {}

    [[nodiscard]] bool failed() const { return failed_; }

    [[nodiscard]] const std::string& error() const { return error_; }

    [[nodiscard]] std::size_t remaining() const { return in_.size() - pos_; }

    [[nodiscard]] bool atEnd() const { return pos_ == in_.size(); }

    void fail(std::string_view why) {
        if (!failed_) {
            failed_ = true;
            error_ = std::string(why) + " at byte " + std::to_string(pos_);
        }
        pos_ = in_.size();
    }

    std::uint8_t u8() {
        if (pos_ >= in_.size()) {
            fail("unexpected end of data");
            return 0;
        }
        return in_[pos_++];
    }

    std::uint64_t u64() {
        std::size_t start = pos_;
        std::uint64_t v = 0;
        for (unsigned shift = 0;; shift += 7) {
            std::uint8_t b = u8();
            if (failed_) {
                return 0;
            }
            if (shift == 63 && b > 1) {
                fail("integer overflow");
                return 0;
            }
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                break;
            }
        }
        if (pos_ - start != ulebSize(v)) {
            fail("non canonical integer");
            return 0;
        }
        return v;
    }

    std::uint32_t u32() {
        std::uint64_t v = u64();
        if (v > 0xFFFFFFFFu) {
            fail("integer out of range");
            return 0;
        }
        return static_cast<std::uint32_t>(v);
    }

    std::int64_t i64() { return unzigzag(u64()); }

    std::uint32_t opt() { return u32() - 1; }

    std::uint64_t fixed64() {
        if (remaining() < 8) {
            fail("unexpected end of data");
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(in_[pos_ + i]) << (8 * i);
        }
        pos_ += 8;
        return v;
    }

    /// A count of elements that each take at least `minBytes`, so it can never exceed the input left.
    std::uint32_t count(std::uint32_t limit, std::size_t minBytes = 1) {
        std::uint64_t n = u64();
        if (n > limit || n > remaining() / minBytes) {
            fail("count out of range");
            return 0;
        }
        return static_cast<std::uint32_t>(n);
    }

    std::string str() {
        std::uint64_t n = u64();
        if (n > remaining()) {
            fail("string longer than the data left");
            return {};
        }
        std::string s(reinterpret_cast<const char*>(in_.data() + pos_), static_cast<std::size_t>(n));
        pos_ += static_cast<std::size_t>(n);
        return s;
    }

    std::span<const std::uint8_t> take(std::size_t n) {
        if (n > remaining()) {
            fail("section longer than the data left");
            return {};
        }
        auto s = in_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

private:
    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
    bool failed_ = false;
    std::string error_;
};

} // namespace quartz::ir::bin
