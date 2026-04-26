#include "ir/Builtins.hpp"
#include "vm/Builtins.hpp"
#include "vm/Sandbox.hpp"

#include <charconv>
#include <cmath>
#include <limits>

namespace quartz::vm {

namespace {

const Str* strOf(const Slot& s) { return static_cast<const Str*>(s.p); }

bool asciiEqualFold(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char x = static_cast<unsigned char>(a[i]);
        unsigned char y = static_cast<unsigned char>(b[i]);
        if (x != y && ((x | 0x20) != (y | 0x20) || (x | 0x20) < 'a' || (x | 0x20) > 'z')) {
            return false;
        }
    }
    return true;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Unicode White_Space, the set Rust's `trim` uses.
bool isSpace(std::uint32_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

// Whether a decimal float the overflow branch of from_chars rejected was too large (true) or too small.
bool tooLarge(std::string_view s) {
    std::size_t i = 0;
    std::int64_t lead = 0;
    bool seen = false;
    bool fraction = false;
    std::int64_t intDigits = 0;
    std::int64_t fracZeros = 0;
    for (; i < s.size() && (isDigit(s[i]) || s[i] == '.'); ++i) {
        if (s[i] == '.') {
            fraction = true;
        } else if (!seen && s[i] == '0') {
            fracZeros += fraction ? 1 : 0;
        } else {
            seen = true;
            intDigits += fraction ? 0 : 1;
        }
    }
    lead = intDigits > 0 ? intDigits - 1 : -(fracZeros + 1);
    std::int64_t exp = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool neg = i < s.size() && s[i] == '-';
        i += i < s.size() && (s[i] == '-' || s[i] == '+') ? 1 : 0;
        for (; i < s.size(); ++i) {
            exp = std::min<std::int64_t>(exp * 10 + (s[i] - '0'), 1'000'000'000);
        }
        exp = neg ? -exp : exp;
    }
    return seen && lead + exp >= 0;
}

Str* copyOf(Sandbox& sb, std::string_view s) { return sb.newStr(s); }

bool setStr(Slot* out, Str* s) {
    out[0].p = s;
    return s != nullptr;
}

std::size_t lastScalarStart(std::string_view s, std::size_t end) {
    std::size_t i = end - 1;
    for (int k = 0; k < 3 && i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80; ++k) {
        --i;
    }
    return i;
}

std::string_view trimmed(std::string_view s, bool start, bool end) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (start && b < e) {
        std::size_t next = b;
        if (!isSpace(decodeUtf8(s, next))) {
            break;
        }
        b = next;
    }
    while (end && e > b) {
        std::size_t at = lastScalarStart(s, e);
        std::size_t probe = at;
        if (at < b || !isSpace(decodeUtf8(s, probe)) || probe != e) {
            break;
        }
        e = at;
    }
    return s.substr(b, e - b);
}

bool trimWith(Sandbox& sb, const Slot* a, Slot* out, bool start, bool end) {
    const Str* s = strOf(a[0]);
    std::string_view t = trimmed(s->view(), start, end);
    if (!Builtins::chargeBytes(sb, s->len)) {
        return false;
    }
    if (t.size() == s->len) {
        out[0].p = const_cast<Str*>(s);
        return true;
    }
    return setStr(out, copyOf(sb, t));
}

bool pushStr(Sandbox& sb, List* l, std::string_view piece) {
    Slot v{};
    v.p = sb.newStr(piece);
    return v.p != nullptr && sb.listPush(l, &v);
}

} // namespace

std::uint32_t decodeUtf8(std::string_view s, std::size_t& i) {
    auto c = static_cast<unsigned char>(s[i]);
    std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || i + n > s.size()) {
        ++i;
        return n == 1 ? c : 0xFFFD;
    }
    std::uint32_t cp = n == 1 ? c : c & (0x7Fu >> n);
    for (std::size_t k = 1; k < n; ++k) {
        auto x = static_cast<unsigned char>(s[i + k]);
        if ((x & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (x & 0x3F);
    }
    static constexpr std::uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return 0xFFFD;
    }
    i += n;
    return cp;
}

bool Builtins::chargeBytes(Sandbox& sb, std::uint64_t bytes) {
    auto units = static_cast<std::int64_t>(bytes >> kFuelByteShift);
    return units == 0 || sb.chargeFuel(units);
}

bool Builtins::run(Sandbox& sb, std::uint32_t index, const Slot* args, Slot* out) {
    static constexpr Fn kTable[] = {
        toInt,   toFloat,          replace, split,       trim,   trimStart, trimEnd,
        repeat,  equalsIgnoreCase, chars,   charFromInt, insert, removeAt,  contains,
        indexOf, reverse,          slice,   insertAll,   sort,
    };
    static_assert(std::size(kTable) == ir::kRuntimeBuiltins);
    return index < std::size(kTable) && kTable[index](sb, args, out);
}

// Decimal only: an optional sign and digits, nothing else; out of range is null.
bool Builtins::toInt(Sandbox& sb, const Slot* a, Slot* out) {
    std::string_view s = strOf(a[0])->view();
    out[0].u = 0;
    out[1].u = 0;
    if (!chargeBytes(sb, s.size())) {
        return false;
    }
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i++] == '-';
    }
    if (i == s.size()) {
        return true;
    }
    const std::uint64_t limit = neg ? 0x8000000000000000ULL : 0x7FFFFFFFFFFFFFFFULL;
    std::uint64_t v = 0;
    for (; i < s.size(); ++i) {
        if (!isDigit(s[i])) {
            return true;
        }
        auto d = static_cast<std::uint64_t>(s[i] - '0');
        if (v > (limit - d) / 10) {
            return true;
        }
        v = v * 10 + d;
    }
    out[0].u = 1;
    out[1].u = neg ? 0 - v : v;
    return true;
}

// Signed float literal syntax (not `1.` or `.5`) correctly rounded, or inf, infinity, nan; overflow is infinity.
bool Builtins::toFloat(Sandbox& sb, const Slot* a, Slot* out) {
    std::string_view s = strOf(a[0])->view();
    out[0].u = 0;
    out[1].u = 0;
    if (!chargeBytes(sb, s.size())) {
        return false;
    }
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        neg = s[i++] == '-';
    }
    std::string_view rest = s.substr(i);
    double value = 0;
    if (asciiEqualFold(rest, "inf") || asciiEqualFold(rest, "infinity")) {
        value = std::numeric_limits<double>::infinity();
    } else if (asciiEqualFold(rest, "nan")) {
        out[0].u = 1;
        out[1].f = std::numeric_limits<double>::quiet_NaN();
        return true;
    } else {
        std::size_t j = 0;
        while (j < rest.size() && isDigit(rest[j])) {
            ++j;
        }
        if (j == 0) {
            return true;
        }
        if (j < rest.size() && rest[j] == '.') {
            std::size_t frac = ++j;
            while (j < rest.size() && isDigit(rest[j])) {
                ++j;
            }
            if (j == frac) {
                return true;
            }
        }
        if (j < rest.size() && (rest[j] == 'e' || rest[j] == 'E')) {
            ++j;
            j += j < rest.size() && (rest[j] == '+' || rest[j] == '-') ? 1 : 0;
            std::size_t expDigits = 0;
            for (; j < rest.size() && isDigit(rest[j]); ++j) {
                ++expDigits;
            }
            if (expDigits == 0) {
                return true;
            }
        }
        if (j != rest.size()) {
            return true;
        }
        auto r = std::from_chars(rest.data(), rest.data() + rest.size(), value, std::chars_format::general);
        if (r.ec == std::errc::result_out_of_range) {
            value = tooLarge(rest) ? std::numeric_limits<double>::infinity() : 0.0;
        } else if (r.ec != std::errc{} || r.ptr != rest.data() + rest.size()) {
            return true;
        }
    }
    out[0].u = 1;
    out[1].f = neg ? -value : value;
    return true;
}

// All non-overlapping matches, left to right; an empty pattern leaves the string as it is.
bool Builtins::replace(Sandbox& sb, const Slot* a, Slot* out) {
    const Str* s = strOf(a[0]);
    std::string_view from = strOf(a[1])->view();
    std::string_view to = strOf(a[2])->view();
    std::string_view text = s->view();
    if (!chargeBytes(sb, text.size())) {
        return false;
    }
    if (from.empty()) {
        out[0].p = const_cast<Str*>(s);
        return true;
    }
    std::uint64_t count = 0;
    for (std::size_t at = text.find(from); at != std::string_view::npos; at = text.find(from, at + from.size())) {
        ++count;
    }
    if (count == 0) {
        out[0].p = const_cast<Str*>(s);
        return true;
    }
    std::uint64_t total = text.size() - count * from.size() + count * to.size();
    if (!chargeBytes(sb, total)) {
        return false;
    }
    Str* r = sb.allocStr(total);
    if (r == nullptr) {
        return false;
    }
    char* w = r->data;
    std::size_t last = 0;
    for (std::size_t at = text.find(from); at != std::string_view::npos; at = text.find(from, at + from.size())) {
        std::memcpy(w, text.data() + last, at - last);
        w += at - last;
        std::memcpy(w, to.data(), to.size());
        w += to.size();
        last = at + from.size();
    }
    std::memcpy(w, text.data() + last, text.size() - last);
    out[0].p = r;
    return true;
}

// Every piece, empty ones included; an empty separator gives one piece per scalar value.
bool Builtins::split(Sandbox& sb, const Slot* a, Slot* out) {
    std::string_view text = strOf(a[0])->view();
    std::string_view sep = strOf(a[1])->view();
    out[0].p = nullptr;
    List* l = sb.newList(ir::kStr);
    if (l == nullptr || !chargeBytes(sb, text.size())) {
        return false;
    }
    if (sep.empty()) {
        for (std::size_t i = 0; i < text.size();) {
            std::size_t b = i;
            decodeUtf8(text, i);
            if (!chargeBytes(sb, 16) || !pushStr(sb, l, text.substr(b, i - b))) {
                return false;
            }
        }
        out[0].p = l;
        return true;
    }
    std::size_t last = 0;
    for (std::size_t at = text.find(sep);; at = text.find(sep, last)) {
        std::size_t end = at == std::string_view::npos ? text.size() : at;
        if (!chargeBytes(sb, 16 + end - last) || !pushStr(sb, l, text.substr(last, end - last))) {
            return false;
        }
        if (at == std::string_view::npos) {
            break;
        }
        last = at + sep.size();
    }
    out[0].p = l;
    return true;
}

bool Builtins::trim(Sandbox& sb, const Slot* a, Slot* out) { return trimWith(sb, a, out, true, true); }

bool Builtins::trimStart(Sandbox& sb, const Slot* a, Slot* out) { return trimWith(sb, a, out, true, false); }

bool Builtins::trimEnd(Sandbox& sb, const Slot* a, Slot* out) { return trimWith(sb, a, out, false, true); }

// A count of zero or less gives the empty string.
bool Builtins::repeat(Sandbox& sb, const Slot* a, Slot* out) {
    const Str* s = strOf(a[0]);
    std::int64_t n = a[1].i;
    if (n <= 0 || s->len == 0) {
        return setStr(out, sb.allocStr(0));
    }
    if (static_cast<std::uint64_t>(n) > 0x7FFFFFFFULL / s->len) {
        out[0].p = nullptr;
        return sb.fail(ir::TrapCode::OutOfMemory, "string too long");
    }
    std::uint64_t total = static_cast<std::uint64_t>(n) * s->len;
    if (!chargeBytes(sb, total)) {
        return false;
    }
    Str* r = sb.allocStr(total);
    if (r == nullptr) {
        return false;
    }
    std::memcpy(r->data, s->data, s->len);
    for (std::uint64_t done = s->len; done < total;) {
        std::uint64_t chunk = std::min(done, total - done);
        std::memcpy(r->data + done, r->data, chunk);
        done += chunk;
    }
    out[0].p = r;
    return true;
}

// ASCII letters only, the same folding as lower() and upper().
bool Builtins::equalsIgnoreCase(Sandbox& sb, const Slot* a, Slot* out) {
    std::string_view x = strOf(a[0])->view();
    std::string_view y = strOf(a[1])->view();
    out[0].u = 0;
    if (x.size() != y.size()) {
        return true;
    }
    if (!chargeBytes(sb, x.size())) {
        return false;
    }
    out[0].u = asciiEqualFold(x, y) ? 1 : 0;
    return true;
}

bool Builtins::chars(Sandbox& sb, const Slot* a, Slot* out) {
    std::string_view text = strOf(a[0])->view();
    out[0].p = nullptr;
    List* l = sb.newList(ir::kChar);
    if (l == nullptr || !chargeBytes(sb, text.size() * 8)) {
        return false;
    }
    for (std::size_t i = 0; i < text.size();) {
        Slot c{};
        c.u = decodeUtf8(text, i);
        if (!sb.listPush(l, &c)) {
            return false;
        }
    }
    out[0].p = l;
    return true;
}

// Unicode scalar values only: surrogates and values past U+10FFFF are null.
bool Builtins::charFromInt(Sandbox&, const Slot* a, Slot* out) {
    std::int64_t v = a[0].i;
    bool ok = v >= 0 && v <= 0x10FFFF && (v < 0xD800 || v > 0xDFFF);
    out[0].u = ok ? 1 : 0;
    out[1].u = ok ? static_cast<std::uint64_t>(v) : 0;
    return true;
}

} // namespace quartz::vm
