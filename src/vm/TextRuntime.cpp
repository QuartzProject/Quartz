#include "vm/Sandbox.hpp"

#include <charconv>
#include <cmath>
#include <string>

namespace quartz::vm {

namespace {

constexpr std::uint32_t kReplacement = 0xFFFD;

bool continuation(unsigned char b, unsigned char lo = 0x80, unsigned char hi = 0xBF) { return b >= lo && b <= hi; }

} // namespace

// Strict UTF-8 (Unicode table 3-7); an ill-formed byte reads as U+FFFD and only that byte is consumed, as in Go.
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

std::string_view shortestF64(double v, char (&buf)[40]) {
    auto r = std::to_chars(buf, buf + sizeof buf - 3, v);
    std::string_view text(buf, static_cast<std::size_t>(r.ptr - buf));
    if (text.find_first_of(".einn") == std::string_view::npos) {
        *r.ptr++ = '.';
        *r.ptr++ = '0';
        text = std::string_view(buf, static_cast<std::size_t>(r.ptr - buf));
    }
    return text;
}

namespace {

void appendChar(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::size_t scalars(std::string_view s) {
    std::size_t n = 0;
    for (char c : s) {
        n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    }
    return n;
}

} // namespace

// Lays out one interpolated value; the spec comes from IR, so every field is clamped rather than trusted.
Str* Sandbox::formatValue(Slot v, std::uint32_t kind, std::int64_t specBits) {
    auto spec = static_cast<std::uint64_t>(specBits);
    std::uint32_t fill = static_cast<std::uint32_t>(spec & 0x1FFFFF);
    if (fill > 0x10FFFF || (fill >= 0xD800 && fill <= 0xDFFF)) {
        fill = ' ';
    }
    auto align = static_cast<std::uint32_t>(spec >> 21 & 3);
    bool plus = (spec >> 23 & 1) != 0;
    bool zero = (spec >> 24 & 1) != 0;
    auto width = static_cast<std::size_t>(spec >> 25 & 0xFFFF);
    auto precision = static_cast<int>(spec >> 41 & 0xFF) - 1;
    auto radix = static_cast<std::uint32_t>(spec >> 49 & 7);
    std::string sign;
    std::string body;
    char buf[640];
    bool number = kind != 3;
    if (kind == 0 || kind == 1) {
        bool neg = kind == 0 && v.i < 0;
        std::uint64_t mag = neg ? 0 - v.u : v.u;
        int base = radix == 1 || radix == 2 ? 16 : radix == 3 ? 2 : radix == 4 ? 8 : 10;
        auto r = std::to_chars(buf, buf + sizeof buf, mag, base);
        body.assign(buf, r.ptr);
        if (radix == 2) {
            for (char& c : body) {
                c = c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c;
            }
        }
        sign = neg ? "-" : plus ? "+" : "";
    } else if (kind == 2) {
        bool finite = std::isfinite(v.f);
        zero = zero && finite;
        if (precision >= 0 && finite) {
            auto r = std::to_chars(buf, buf + sizeof buf, std::fabs(v.f), std::chars_format::fixed, precision);
            body.assign(buf, r.ptr);
        } else {
            char small[40];
            body = shortestF64(std::fabs(v.f), small);
        }
        sign = std::signbit(v.f) && !std::isnan(v.f) ? "-" : plus ? "+" : "";
    } else {
        body = static_cast<const Str*>(v.p)->view();
    }
    std::size_t len = scalars(sign) + scalars(body);
    std::string out;
    if (len >= width) {
        out = sign + body;
    } else if (zero && number) {
        out = sign + std::string(width - len, '0') + body;
    } else {
        std::size_t pad = width - len;
        std::uint32_t side = align != 0 ? align : number ? 2 : 1;
        std::size_t before = side == 2 ? pad : side == 3 ? pad / 2 : 0;
        for (std::size_t i = 0; i < before; ++i) {
            appendChar(out, fill);
        }
        out += sign;
        out += body;
        for (std::size_t i = before; i < pad; ++i) {
            appendChar(out, fill);
        }
    }
    return newStr(out);
}

} // namespace quartz::vm
