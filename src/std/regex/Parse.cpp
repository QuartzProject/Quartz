#include "std/regex/Ast.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace quartz::stdlib::regex {

namespace {

struct Flags {
    bool fold = false;
    bool multiLine = false;
    bool dotAll = false;
};

constexpr std::uint32_t kFlagsOnly = kNoPos - 1;

std::vector<Range> normalized(std::vector<Range> r) {
    std::sort(r.begin(), r.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });
    std::vector<Range> out;
    for (const Range& x : r) {
        if (!out.empty() && x.lo <= out.back().hi + 1) {
            out.back().hi = std::max(out.back().hi, x.hi);
        } else {
            out.push_back(x);
        }
    }
    return out;
}

std::vector<Range> complement(const std::vector<Range>& r) {
    std::vector<Range> out;
    std::uint32_t next = 0;
    for (const Range& x : r) {
        if (x.lo > next) {
            out.push_back({next, x.lo - 1});
        }
        next = x.hi + 1;
    }
    if (next <= kMaxCodePoint) {
        out.push_back({next, kMaxCodePoint});
    }
    return out;
}

void addAsciiFolds(std::vector<Range>& r) {
    std::size_t n = r.size();
    for (std::size_t k = 0; k < n; ++k) {
        Range x = r[k];
        std::uint32_t a = std::max<std::uint32_t>(x.lo, 'A');
        std::uint32_t b = std::min<std::uint32_t>(x.hi, 'Z');
        if (a <= b) {
            r.push_back({a + 32, b + 32});
        }
        a = std::max<std::uint32_t>(x.lo, 'a');
        b = std::min<std::uint32_t>(x.hi, 'z');
        if (a <= b) {
            r.push_back({a - 32, b - 32});
        }
    }
}

std::vector<Range> perlClass(char c) {
    std::vector<Range> r;
    switch (c | 0x20) {
    case 'd':
        r = {{'0', '9'}};
        break;
    case 'w':
        r = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
        break;
    default:
        r = {{'\t', '\r'}, {' ', ' '}};
        break;
    }
    return c >= 'A' && c <= 'Z' ? complement(r) : r;
}

bool isPerlClass(std::uint32_t c) { return c == 'd' || c == 'D' || c == 'w' || c == 'W' || c == 's' || c == 'S'; }

int hexDigit(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

bool isAsciiAlnum(std::uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    std::expected<Ast, Error> run() {
        for (std::size_t k = 0; k < s_.size();) {
            std::size_t at = k;
            if (decode(s_, k) == 0xFFFD && k == at + 1) {
                return std::unexpected(Error{"the pattern is not valid UTF-8", at});
            }
        }
        ast_.names.emplace_back();
        Flags flags;
        std::uint32_t root = parseAlt(flags);
        if (!error_ && !eof()) {
            fail("unmatched ')'", i_);
        }
        if (error_) {
            return std::unexpected(std::move(*error_));
        }
        ast_.root = root;
        return std::move(ast_);
    }

private:
    std::uint32_t fail(std::string message, std::size_t at) {
        if (!error_) {
            error_ = Error{std::move(message), at};
        }
        return kNoPos;
    }

    [[nodiscard]] bool eof() const { return i_ >= s_.size(); }

    [[nodiscard]] bool at(char c) const { return i_ < s_.size() && s_[i_] == c; }

    std::uint32_t next() { return decode(s_, i_); }

    std::uint32_t add(Node n) {
        ast_.nodes.push_back(std::move(n));
        return static_cast<std::uint32_t>(ast_.nodes.size() - 1);
    }

    std::uint32_t setNode(std::vector<Range> ranges, std::size_t pos) {
        ast_.sets.push_back(normalized(std::move(ranges)));
        Node n{.kind = NodeKind::Set, .set = static_cast<std::uint32_t>(ast_.sets.size() - 1)};
        n.pos = static_cast<std::uint32_t>(pos);
        return add(std::move(n));
    }

    std::uint32_t literal(std::uint32_t c, const Flags& flags, std::size_t pos) {
        std::vector<Range> r{{c, c}};
        if (flags.fold) {
            addAsciiFolds(r);
        }
        return setNode(std::move(r), pos);
    }

    std::uint32_t assertion(Assert a, std::size_t pos) {
        Node n{.kind = NodeKind::Assert, .assertion = a};
        n.pos = static_cast<std::uint32_t>(pos);
        return add(std::move(n));
    }

    std::uint32_t sequence(std::vector<std::uint32_t>& items) {
        if (items.size() == 1) {
            std::uint32_t only = items[0];
            items.clear();
            return only;
        }
        Node n{.kind = items.empty() ? NodeKind::Empty : NodeKind::Concat};
        n.kids = std::move(items);
        items.clear();
        return add(std::move(n));
    }

    std::uint32_t parseAlt(Flags& flags) {
        std::vector<std::uint32_t> branches;
        std::vector<std::uint32_t> items;
        while (!eof() && !at(')')) {
            if (at('|')) {
                ++i_;
                branches.push_back(sequence(items));
                continue;
            }
            std::size_t start = i_;
            std::uint32_t atom = parseAtom(flags);
            if (atom == kFlagsOnly) {
                continue;
            }
            if (atom != kNoPos) {
                atom = parseQuantifier(atom, start);
            }
            if (atom == kNoPos) {
                return kNoPos;
            }
            items.push_back(atom);
        }
        branches.push_back(sequence(items));
        if (branches.size() == 1) {
            return branches[0];
        }
        Node n{.kind = NodeKind::Alternate};
        n.kids = std::move(branches);
        return add(std::move(n));
    }

    // `{n}`, `{n,}` or `{n,m}` at `from`; anything else is a literal brace, as in RE2.
    bool repeatAt(std::size_t from, std::uint32_t& lo, std::uint32_t& hi, std::size_t& end) const {
        std::size_t k = from + 1;
        auto number = [&](std::uint32_t& v) {
            std::size_t first = k;
            std::uint64_t n = 0;
            while (k < s_.size() && s_[k] >= '0' && s_[k] <= '9') {
                n = std::min<std::uint64_t>(n * 10 + static_cast<std::uint64_t>(s_[k] - '0'), kNoPos - 1);
                ++k;
            }
            v = static_cast<std::uint32_t>(n);
            return k > first;
        };
        if (!number(lo)) {
            return false;
        }
        hi = lo;
        if (k < s_.size() && s_[k] == ',') {
            ++k;
            if (!number(hi)) {
                hi = kNoPos;
            }
        }
        if (k >= s_.size() || s_[k] != '}') {
            return false;
        }
        end = k + 1;
        return true;
    }

    [[nodiscard]] bool quantifierAt(std::size_t k) const {
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        std::size_t end = 0;
        return k < s_.size() &&
               (s_[k] == '*' || s_[k] == '+' || s_[k] == '?' || (s_[k] == '{' && repeatAt(k, lo, hi, end)));
    }

    std::uint32_t parseQuantifier(std::uint32_t atom, std::size_t atomAt) {
        if (!quantifierAt(i_)) {
            return atom;
        }
        std::size_t qAt = i_;
        std::uint32_t lo = 0;
        std::uint32_t hi = kNoPos;
        char q = s_[i_];
        if (q == '{') {
            std::size_t end = 0;
            repeatAt(i_, lo, hi, end);
            if (lo > kMaxRepeat || (hi != kNoPos && hi > kMaxRepeat)) {
                return fail(std::format("repetition count above {}", kMaxRepeat), qAt);
            }
            if (hi < lo) {
                return fail("invalid repetition: the minimum is above the maximum", qAt);
            }
            i_ = end;
        } else {
            lo = q == '+' ? 1 : 0;
            hi = q == '?' ? 1 : kNoPos;
            ++i_;
        }
        if (ast_.nodes[atom].kind == NodeKind::Assert && s_[atomAt] != '(') {
            return fail("nothing to repeat", atomAt);
        }
        bool greedy = !at('?');
        i_ += greedy ? 0 : 1;
        if (quantifierAt(i_)) {
            return fail("nested quantifier", i_);
        }
        if (lo == 1 && hi == 1) {
            return atom;
        }
        Node n{.kind = NodeKind::Repeat, .greedy = greedy, .min = lo, .max = hi};
        n.pos = static_cast<std::uint32_t>(qAt);
        n.kids = {atom};
        return add(std::move(n));
    }

    std::uint32_t parseAtom(Flags& flags) {
        std::size_t start = i_;
        std::uint32_t c = next();
        switch (c) {
        case '(':
            return parseGroup(flags, start);
        case '[':
            return parseClass(flags, start);
        case '.': {
            Node n{.kind = flags.dotAll ? NodeKind::Any : NodeKind::AnyNotNl};
            n.pos = static_cast<std::uint32_t>(start);
            return add(std::move(n));
        }
        case '^':
            return assertion(flags.multiLine ? Assert::BeginLine : Assert::Begin, start);
        case '$':
            return assertion(flags.multiLine ? Assert::EndLine : Assert::End, start);
        case '\\':
            return parseEscape(flags, start);
        case '*':
        case '+':
        case '?':
            return fail("nothing to repeat", start);
        case '{':
            if (quantifierAt(start)) {
                return fail("nothing to repeat", start);
            }
            return literal(c, flags, start);
        default:
            return literal(c, flags, start);
        }
    }

    std::uint32_t parseEscape(const Flags& flags, std::size_t start) {
        if (eof()) {
            return fail("trailing backslash", start);
        }
        std::uint32_t c = next();
        if (isPerlClass(c)) {
            return setNode(perlClass(static_cast<char>(c)), start);
        }
        switch (c) {
        case 'b':
            return assertion(Assert::Word, start);
        case 'B':
            return assertion(Assert::NotWord, start);
        case 'A':
            return assertion(Assert::Begin, start);
        case 'z':
            return assertion(Assert::End, start);
        default:
            break;
        }
        std::uint32_t cp = escapedChar(c, start);
        return cp == kNoPos ? kNoPos : literal(cp, flags, start);
    }

    // The code point an escape outside \d, \w, \s and the assertions stands for.
    std::uint32_t escapedChar(std::uint32_t c, std::size_t start) {
        switch (c) {
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case 'f':
            return '\f';
        case 'v':
            return '\v';
        case 'a':
            return '\a';
        case 'x':
            return hexEscape(start);
        case 'p':
        case 'P':
            return fail("Unicode classes such as \\p{L} are not supported", start);
        default:
            break;
        }
        if (c >= '1' && c <= '9') {
            return fail("backreferences are not supported (they make matching non-linear)", start);
        }
        if (c == '0') {
            return fail("octal escapes are not supported; write \\x00", start);
        }
        if (c < 0x80 && !isAsciiAlnum(c)) {
            return c;
        }
        return fail("unknown escape", start);
    }

    std::uint32_t hexEscape(std::size_t start) {
        std::uint32_t v = 0;
        if (at('{')) {
            ++i_;
            std::size_t digits = 0;
            while (!eof() && hexDigit(s_[i_]) >= 0) {
                v = std::min<std::uint32_t>(v * 16 + static_cast<std::uint32_t>(hexDigit(s_[i_])), 0x1000000);
                ++i_;
                ++digits;
            }
            if (digits == 0 || !at('}')) {
                return fail("bad \\x{...} escape", start);
            }
            ++i_;
        } else {
            for (int k = 0; k < 2; ++k) {
                int d = eof() ? -1 : hexDigit(s_[i_]);
                if (d < 0) {
                    return fail("\\x takes two hex digits or \\x{...}", start);
                }
                v = v * 16 + static_cast<std::uint32_t>(d);
                ++i_;
            }
        }
        if (v > kMaxCodePoint || (v >= 0xD800 && v <= 0xDFFF)) {
            return fail("escape is not a Unicode scalar value", start);
        }
        return v;
    }

    std::uint32_t parseGroup(Flags& flags, std::size_t start) {
        if (depth_ >= kMaxDepth) {
            return fail(std::format("groups nested more than {} deep", kMaxDepth), start);
        }
        Flags inner = flags;
        bool capture = true;
        std::string name;
        if (at('?')) {
            ++i_;
            char k = eof() ? '\0' : s_[i_];
            char k2 = i_ + 1 < s_.size() ? s_[i_ + 1] : '\0';
            if (k == '=' || k == '!' || (k == '<' && (k2 == '=' || k2 == '!'))) {
                return fail("lookaround is not supported (it makes matching non-linear)", start);
            }
            if (k == 'P' && k2 == '=') {
                return fail("backreferences are not supported (they make matching non-linear)", start);
            }
            if (k == '<' || (k == 'P' && k2 == '<')) {
                i_ += k == 'P' ? 2 : 1;
                if (!groupName(name, start)) {
                    return kNoPos;
                }
            } else {
                std::optional<bool> scoped = parseFlags(inner, start);
                if (!scoped) {
                    return kNoPos;
                }
                if (!*scoped) {
                    flags = inner;
                    return kFlagsOnly;
                }
                capture = false;
            }
        }
        std::uint32_t index = 0;
        if (capture) {
            if (ast_.names.size() > kMaxGroups) {
                return fail(std::format("more than {} groups", kMaxGroups), start);
            }
            index = static_cast<std::uint32_t>(ast_.names.size());
            ast_.names.push_back(std::move(name));
        }
        ++depth_;
        std::uint32_t body = parseAlt(inner);
        --depth_;
        if (body == kNoPos) {
            return kNoPos;
        }
        if (!at(')')) {
            return fail("missing ')'", start);
        }
        ++i_;
        if (!capture) {
            return body;
        }
        Node n{.kind = NodeKind::Group, .capture = index};
        n.pos = static_cast<std::uint32_t>(start);
        n.kids = {body};
        return add(std::move(n));
    }

    bool groupName(std::string& name, std::size_t start) {
        while (!eof() && !at('>')) {
            char c = s_[i_];
            bool ok =
                c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (!name.empty() && c >= '0' && c <= '9');
            if (!ok) {
                fail("invalid group name", start);
                return false;
            }
            name += c;
            ++i_;
        }
        if (eof() || name.empty()) {
            fail(eof() ? "missing '>' after the group name" : "empty group name", start);
            return false;
        }
        ++i_;
        if (std::find(ast_.names.begin(), ast_.names.end(), name) != ast_.names.end()) {
            fail("duplicate group name '" + name + "'", start);
            return false;
        }
        return true;
    }

    // `(?ims-ims)` sets flags for the rest of the group (false); `(?ims-ims:...)` scopes them to its body (true).
    std::optional<bool> parseFlags(Flags& flags, std::size_t start) {
        bool negate = false;
        bool letterSinceSign = false;
        bool any = false;
        while (!eof()) {
            char c = s_[i_++];
            if (c == ')' || c == ':') {
                if ((!any && !(c == ':' && !negate)) || (negate && !letterSinceSign)) {
                    break;
                }
                return c == ':';
            }
            if (c == '-' && !negate) {
                negate = true;
                letterSinceSign = false;
                continue;
            }
            bool* flag = c == 'i' ? &flags.fold : c == 'm' ? &flags.multiLine : c == 's' ? &flags.dotAll : nullptr;
            if (flag == nullptr) {
                fail("unknown group flag or syntax", start);
                return std::nullopt;
            }
            *flag = !negate;
            any = true;
            letterSinceSign = true;
        }
        fail("bad group flags", start);
        return std::nullopt;
    }

    // One element of a class: a code point (true) or a Perl class appended to `r` (false, or an error).
    bool classItem(std::uint32_t& cp, std::vector<Range>& r) {
        std::size_t start = i_;
        std::uint32_t c = next();
        if (c != '\\') {
            cp = c;
            return true;
        }
        if (eof()) {
            fail("trailing backslash", start);
            return false;
        }
        c = next();
        if (isPerlClass(c)) {
            auto p = perlClass(static_cast<char>(c));
            r.insert(r.end(), p.begin(), p.end());
            return false;
        }
        if (c == 'b' || c == 'B' || c == 'A' || c == 'z') {
            fail("assertions cannot be part of a class", start);
            return false;
        }
        cp = escapedChar(c, start);
        return cp != kNoPos;
    }

    std::uint32_t parseClass(const Flags& flags, std::size_t start) {
        bool negate = at('^');
        i_ += negate ? 1 : 0;
        std::vector<Range> r;
        bool first = true;
        for (;;) {
            if (eof()) {
                return fail("missing ']'", start);
            }
            std::size_t itemAt = i_;
            if (at(']') && !first) {
                ++i_;
                break;
            }
            if (at('[') && i_ + 1 < s_.size() && s_[i_ + 1] == ':') {
                return fail("POSIX classes such as [:alpha:] are not supported", i_);
            }
            first = false;
            std::uint32_t lo = 0;
            bool single = classItem(lo, r);
            if (error_) {
                return kNoPos;
            }
            bool range = i_ + 1 < s_.size() && s_[i_] == '-' && s_[i_ + 1] != ']';
            if (range && !single) {
                return fail("invalid class range", itemAt);
            }
            if (!single) {
                continue;
            }
            std::uint32_t hi = lo;
            if (range) {
                ++i_;
                bool hiSingle = classItem(hi, r);
                if (error_) {
                    return kNoPos;
                }
                if (!hiSingle || hi < lo) {
                    return fail("invalid class range", itemAt);
                }
            }
            r.push_back({lo, hi});
        }
        if (flags.fold) {
            addAsciiFolds(r);
        }
        r = normalized(std::move(r));
        return setNode(negate ? complement(r) : std::move(r), start);
    }

    std::string_view s_;
    std::size_t i_ = 0;
    Ast ast_;
    std::uint32_t depth_ = 0;
    std::optional<Error> error_;
};

} // namespace

std::expected<Ast, Error> parse(std::string_view pattern) {
    if (pattern.size() > kMaxPatternBytes) {
        return std::unexpected(Error{std::format("pattern longer than {} bytes", kMaxPatternBytes), 0});
    }
    return Parser(pattern).run();
}

} // namespace quartz::stdlib::regex
