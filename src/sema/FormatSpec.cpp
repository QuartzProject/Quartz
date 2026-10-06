#include "sema/SemaImpl.hpp"

#include <format>

namespace quartz::sema {

namespace {

// Decodes one UTF-8 scalar from a spec that came from valid UTF-8 source text.
std::uint32_t takeChar(std::string_view s, std::size_t& i) {
    auto b = static_cast<unsigned char>(s[i]);
    std::size_t len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    std::uint32_t cp = len == 1 ? b : b & (0x7Fu >> len);
    for (std::size_t k = 1; k < len && i + k < s.size(); ++k) {
        cp = cp << 6 | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    }
    i += len;
    return cp;
}

bool alignChar(std::uint32_t c) { return c == '<' || c == '>' || c == '^'; }

// Text ending in a regex atom (`\d`, `]`, `)`, `.`): a `{4}` after it was meant as a quantifier, not the number 4.
bool quantifies(std::string_view before) {
    if (before.empty()) {
        return false;
    }
    char last = before.back();
    bool escaped = before.size() >= 2 && before[before.size() - 2] == '\\' &&
                   ((last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z'));
    return escaped || last == ']' || last == ')' || last == '.';
}

} // namespace

// `[[fill]align][+][0][width][.precision][x|X|b|o]`, packed as ir::Intrinsic::StrFormat documents.
std::uint64_t Checker::formatSpec(std::string_view spec, TypeId t, Span span) {
    std::size_t i = 0;
    std::uint64_t fill = ' ';
    std::uint64_t align = 0;
    std::size_t first = 0;
    std::uint32_t c0 = spec.empty() ? 0 : takeChar(spec, first);
    std::size_t second = first;
    std::uint32_t c1 = first < spec.size() ? takeChar(spec, second) : 0;
    auto alignOf = [](std::uint32_t c) { return c == '<' ? 1u : c == '>' ? 2u : 3u; };
    if (first < spec.size() && alignChar(c1)) {
        fill = c0;
        align = alignOf(c1);
        i = second;
    } else if (alignChar(c0)) {
        align = alignOf(c0);
        i = first;
    }
    bool plus = i < spec.size() && spec[i] == '+';
    i += plus ? 1 : 0;
    bool zero = i < spec.size() && spec[i] == '0';
    i += zero ? 1 : 0;
    auto number = [&](std::uint64_t& out) {
        std::size_t begin = i;
        out = 0;
        while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9' && out < 100000) {
            out = out * 10 + static_cast<std::uint64_t>(spec[i++] - '0');
        }
        return i > begin;
    };
    std::uint64_t width = 0;
    number(width);
    std::uint64_t precision = 0;
    bool hasPrecision = i < spec.size() && spec[i] == '.';
    if (hasPrecision) {
        ++i;
        hasPrecision = number(precision);
        if (!hasPrecision) {
            i = spec.size() + 1; // `.` without digits
        }
    }
    std::uint64_t radix = 0;
    if (i < spec.size()) {
        std::string_view kinds = "xXbo";
        std::size_t k = kinds.find(spec[i]);
        radix = k == std::string_view::npos ? 0 : k + 1;
        i += radix != 0 ? 1 : 0;
    }
    if (i != spec.size() || fill == '}' || fill == '{') {
        diags_.error(span, std::format("invalid format spec '{}': expected [[fill]align][+][0][width][.precision]"
                                       "[x|X|b|o]",
                                       spec));
        return 0;
    }
    bool integer = types_->isInteger(t);
    bool numeric = integer || types_->isFloat(t);
    if (width > 1024 || precision > 64) {
        diags_.error(span, "a format width is at most 1024 and a precision at most 64");
    } else if (radix != 0 && !integer) {
        diags_.error(span, std::format("'{}' formats integers only, not '{}'", spec[spec.size() - 1], typeName(t)));
    } else if (hasPrecision && !types_->isFloat(t)) {
        diags_.error(span, std::format("a precision formats floats only, not '{}'", typeName(t)));
    } else if ((plus || zero) && !numeric) {
        diags_.error(span, std::format("'+' and '0' format numbers only, not '{}'", typeName(t)));
    }
    return fill | align << 21 | std::uint64_t{plus} << 23 | std::uint64_t{zero} << 24 | width << 25 |
           (hasPrecision ? precision + 1 : 0) << 41 | radix << 49 | std::uint64_t{1} << 62;
}

TypeId Checker::checkInterp(InterpExpr* e) {
    std::vector<std::uint64_t> formats(e->exprs.size(), 0);
    for (std::size_t k = 0; k < e->exprs.size(); ++k) {
        Expr* part = e->exprs[k];
        bool spec = k < e->specs.size() && !e->specs[k].empty();
        if (part->kind == ExprKind::IntLit && !spec && k < e->texts.size() && quantifies(e->texts[k]) &&
            lintable(currentFile_)) {
            diags_.warning(part->span, rule::kLiteralInterpolation,
                           std::format("'{{{}}}' interpolates a number literal; write '\\{{' for a brace, or use a raw "
                                       "string (r\"...\")",
                                       static_cast<IntLitExpr*>(part)->value));
        }
        TypeId t = checkValue(part, ir::kError);
        if (t == ir::kError) {
            continue;
        }
        if (!implements(t, langTrait(LangTrait::Show))) {
            diags_.error(part->span, std::format("cannot interpolate a value of type '{}'; it does not implement Show",
                                                 typeName(t)));
            continue;
        }
        if (k < e->specs.size() && !e->specs[k].empty()) {
            formats[k] = formatSpec(e->specs[k], t, part->span);
        }
    }
    e->formats = arena_.copy(formats);
    return ir::kStr;
}

} // namespace quartz::sema
