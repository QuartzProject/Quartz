#include "std/Builder.hpp"
#include "std/Natives.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <limits>
#include <vector>

namespace quartz::stdlib {

namespace {

constexpr int kMaxDepth = 256;

bool validUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > s.size()) {
            return false;
        }
        std::uint32_t cp = n == 1 ? c : c & (0x7F >> n);
        for (std::size_t k = 1; k < n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        static constexpr std::uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += n;
    }
    return true;
}

// One NaN for every payload, as everywhere else in Quartz (D24).
double canonical(double d) { return d != d ? std::numeric_limits<double>::quiet_NaN() : d; }

class Reader {
public:
    Reader(Builder& b, std::span<const std::uint8_t> in) : b_(b), in_(in) {}

    std::string error;

    bool parse(vm::Slot* out) {
        if (!value(out, 0)) {
            return false;
        }
        return pos_ == in_.size() || fail("trailing bytes after the value");
    }

private:
    bool fail(std::string_view what) {
        if (error.empty()) {
            error = std::format("msgpack: {} at byte {}", what, pos_);
        }
        return false;
    }

    bool take(std::size_t n, std::uint64_t& v) {
        if (in_.size() - pos_ < n) {
            return fail("truncated input");
        }
        v = 0;
        for (std::size_t k = 0; k < n; ++k) {
            v = (v << 8) | in_[pos_++];
        }
        return true;
    }

    bool text(std::size_t n, vm::Slot* out, bool binary) {
        if (in_.size() - pos_ < n) {
            return fail("truncated string");
        }
        std::string_view s(reinterpret_cast<const char*>(in_.data() + pos_), n);
        if (!validUtf8(s)) {
            return fail(binary ? "binary data that is not UTF-8 text" : "a string that is not UTF-8");
        }
        pos_ += n;
        return b_.setStr(out, s);
    }

    bool array(std::size_t n, vm::Slot* out, int depth) {
        if (in_.size() - pos_ < n) {
            return fail("truncated array");
        }
        vm::List* l = b_.newList(out);
        if (l == nullptr) {
            return false;
        }
        for (std::size_t k = 0; k < n; ++k) {
            vm::Slot item[2];
            if (!value(item, depth + 1) || !b_.push(l, item)) {
                return false;
            }
        }
        return true;
    }

    bool map(std::size_t n, vm::Slot* out, int depth) {
        if ((in_.size() - pos_) / 2 < n) {
            return fail("truncated map");
        }
        vm::Map* m = b_.newTable(out);
        if (m == nullptr) {
            return false;
        }
        for (std::size_t k = 0; k < n; ++k) {
            vm::Slot key[2];
            vm::Slot item[2];
            if (!value(key, depth + 1)) {
                return false;
            }
            if (static_cast<Tag>(key[0].u) != Tag::Str) {
                return fail("a map key that is not a string");
            }
            if (!value(item, depth + 1)) {
                return false;
            }
            vm::Slot ks{};
            ks.p = key[1].p;
            if (!b_.sandbox().mapSet(m, &ks, item)) {
                return false;
            }
        }
        return true;
    }

    bool value(vm::Slot* out, int depth) {
        if (depth > kMaxDepth) {
            return fail("nesting deeper than 256");
        }
        std::uint64_t t = 0;
        if (!take(1, t)) {
            return false;
        }
        std::uint64_t v = 0;
        if (t <= 0x7F || t >= 0xE0) {
            Builder::setInt(out, static_cast<std::int8_t>(t));
            return true;
        }
        if (t >= 0x80 && t <= 0x8F) {
            return map(t & 15, out, depth);
        }
        if (t >= 0x90 && t <= 0x9F) {
            return array(t & 15, out, depth);
        }
        if (t >= 0xA0 && t <= 0xBF) {
            return text(t & 31, out, false);
        }
        switch (t) {
        case 0xC0:
            Builder::setNull(out);
            return true;
        case 0xC2:
        case 0xC3:
            Builder::setBool(out, t == 0xC3);
            return true;
        case 0xC4:
        case 0xC5:
        case 0xC6:
            return take(std::size_t{1} << (t - 0xC4), v) && text(v, out, true);
        case 0xCA:
            if (!take(4, v)) {
                return false;
            }
            Builder::setFloat(out, canonical(std::bit_cast<float>(static_cast<std::uint32_t>(v))));
            return true;
        case 0xCB:
            if (!take(8, v)) {
                return false;
            }
            Builder::setFloat(out, canonical(std::bit_cast<double>(v)));
            return true;
        case 0xCC:
        case 0xCD:
        case 0xCE:
        case 0xCF:
            if (!take(std::size_t{1} << (t - 0xCC), v)) {
                return false;
            }
            if (v > static_cast<std::uint64_t>(INT64_MAX)) {
                Builder::setFloat(out, static_cast<double>(v));
            } else {
                Builder::setInt(out, static_cast<std::int64_t>(v));
            }
            return true;
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3: {
            std::size_t n = std::size_t{1} << (t - 0xD0);
            if (!take(n, v)) {
                return false;
            }
            int shift = static_cast<int>(64 - 8 * n);
            Builder::setInt(out, static_cast<std::int64_t>(v << shift) >> shift);
            return true;
        }
        case 0xD9:
        case 0xDA:
        case 0xDB:
            return take(std::size_t{1} << (t - 0xD9), v) && text(v, out, false);
        case 0xDC:
        case 0xDD:
            return take(t == 0xDC ? 2 : 4, v) && array(v, out, depth);
        case 0xDE:
        case 0xDF:
            return take(t == 0xDE ? 2 : 4, v) && map(v, out, depth);
        default:
            return fail(std::format("unsupported type byte 0x{:02x} (extension types are not supported)", t));
        }
    }

    Builder& b_;
    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
};

void put(std::string& out, std::uint8_t tag, std::uint64_t v, int bytes) {
    out += static_cast<char>(tag);
    for (int k = bytes - 1; k >= 0; --k) {
        out += static_cast<char>(v >> (8 * k));
    }
}

void header(std::string& out, std::uint64_t n, std::uint8_t fix, std::uint64_t fixMax, std::uint8_t t16) {
    if (n <= fixMax) {
        out += static_cast<char>(fix | n);
    } else if (n <= 0xFFFF) {
        put(out, t16, n, 2);
    } else {
        put(out, t16 + 1, n, 4);
    }
}

// Fuel per value, so a list holding the same list many times over cannot make it run unpaid.
bool write(vm::HostCall& call, std::string& out, const vm::Slot* v, int depth) {
    if (depth > kMaxDepth) {
        return misuse(call, "toMsgPack: the value nests deeper than 256 (or contains itself)");
    }
    if (!call.charge(1)) {
        return false;
    }
    switch (static_cast<Tag>(v[0].u)) {
    case Tag::Null:
        out += '\xC0';
        return true;
    case Tag::Bool:
        out += v[1].u != 0 ? '\xC3' : '\xC2';
        return true;
    case Tag::Int: {
        std::int64_t i = v[1].i;
        if (i >= -32 && i <= 127) {
            out += static_cast<char>(i);
        } else if (i >= 0) {
            int bytes = i <= 0xFF ? 1 : i <= 0xFFFF ? 2 : i <= 0xFFFFFFFF ? 4 : 8;
            put(out, static_cast<std::uint8_t>(0xCC + std::countr_zero(static_cast<unsigned>(bytes))),
                static_cast<std::uint64_t>(i), bytes);
        } else {
            int bytes = i >= -128 ? 1 : i >= -32768 ? 2 : i >= INT32_MIN ? 4 : 8;
            put(out, static_cast<std::uint8_t>(0xD0 + std::countr_zero(static_cast<unsigned>(bytes))),
                static_cast<std::uint64_t>(i), bytes);
        }
        return true;
    }
    case Tag::Float:
        put(out, 0xCB, std::bit_cast<std::uint64_t>(v[1].f), 8);
        return true;
    case Tag::Str: {
        std::string_view s = strArg(v[1]);
        if (s.size() <= 31) {
            out += static_cast<char>(0xA0 | s.size());
        } else if (s.size() <= 0xFF) {
            put(out, 0xD9, s.size(), 1);
        } else if (s.size() <= 0xFFFF) {
            put(out, 0xDA, s.size(), 2);
        } else {
            put(out, 0xDB, s.size(), 4);
        }
        out += s;
        return true;
    }
    case Tag::List: {
        const auto* l = static_cast<const vm::List*>(v[1].p);
        header(out, l->len, 0x90, 15, 0xDC);
        for (std::uint32_t i = 0; i < l->len; ++i) {
            if (!write(call, out, l->data + static_cast<std::size_t>(i) * l->elemSlots, depth + 1)) {
                return false;
            }
        }
        return true;
    }
    case Tag::Table: {
        const auto* m = static_cast<const vm::Map*>(v[1].p);
        std::vector<const vm::Slot*> entries;
        std::uint32_t stride = 1u + m->valueSlots;
        for (std::uint32_t i = 0; i < m->cap; ++i) {
            if (m->hashes[i] >= 2) {
                entries.push_back(m->entries + static_cast<std::size_t>(i) * stride);
            }
        }
        std::sort(entries.begin(), entries.end(),
                  [](const vm::Slot* a, const vm::Slot* b) { return strArg(a[0]) < strArg(b[0]); });
        header(out, entries.size(), 0x80, 15, 0xDE);
        for (const vm::Slot* e : entries) {
            vm::Slot key[2];
            key[0].u = static_cast<std::uint64_t>(Tag::Str);
            key[1] = e[0];
            if (!write(call, out, key, depth + 1) || !write(call, out, e + 1, depth + 1)) {
                return false;
            }
        }
        return true;
    }
    }
    return true;
}

} // namespace

bool parseMsgPack(vm::HostCall& call, void* user) {
    Builder b(call.sandbox, *static_cast<const NativeCtx*>(user)->data);
    auto in = bytesArg(call.args[0]);
    if (!call.charge(static_cast<std::int64_t>(in.size() / 2 + 1))) {
        return false;
    }
    Reader r(b, in);
    vm::Slot value[2];
    if (!r.parse(value)) {
        return r.error.empty() ? false : b.err(call.out, r.error);
    }
    return b.ok(call.out, value);
}

bool toMsgPack(vm::HostCall& call, void*) {
    std::string out;
    if (!write(call, out, call.args, 0)) {
        return false;
    }
    return putStr(call, out, &call.result);
}

} // namespace quartz::stdlib
