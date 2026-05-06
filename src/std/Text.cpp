#include "std/Natives.hpp"
#include "vm/Builtins.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace quartz::stdlib {

namespace {

std::vector<std::uint32_t> scalars(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        out.push_back(vm::decodeUtf8(s, i));
    }
    return out;
}

enum class Tok : std::uint8_t { Literal, One, Class, Star, Deep, DeepSep };

struct Token {
    Tok kind;
    std::uint32_t c = 0;      // Literal
    bool negated = false;     // Class
    std::uint32_t ranges = 0; // Class: index of its first range in Glob::ranges
    std::uint32_t count = 0;
};

struct Glob {
    std::vector<Token> tokens;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
};

constexpr std::uint32_t kNoSep = 0xFFFFFFFF;

// `[` without its closing `]` is a literal `[`, like most shells.
Glob compile(const std::vector<std::uint32_t>& p, std::uint32_t sep) {
    Glob g;
    for (std::size_t i = 0; i < p.size(); ++i) {
        std::uint32_t c = p[i];
        if (c == '\\' && i + 1 < p.size()) {
            g.tokens.push_back({Tok::Literal, p[++i]});
        } else if (c == '?') {
            g.tokens.push_back({Tok::One});
        } else if (c == '*') {
            bool deep = i + 1 < p.size() && p[i + 1] == '*';
            if (!deep || sep == kNoSep) {
                i += deep ? 1 : 0;
                g.tokens.push_back({Tok::Star});
                continue;
            }
            ++i;
            bool segmentStart = i == 1 || p[i - 2] == sep;
            if (segmentStart && i + 1 < p.size() && p[i + 1] == sep) {
                ++i;
                g.tokens.push_back({Tok::DeepSep});
            } else {
                g.tokens.push_back({Tok::Deep});
            }
        } else if (c == '[') {
            std::size_t j = i + 1;
            bool negated = j < p.size() && (p[j] == '!' || p[j] == '^');
            j += negated ? 1 : 0;
            Token t{Tok::Class, 0, negated, static_cast<std::uint32_t>(g.ranges.size())};
            bool first = true;
            while (j < p.size() && (p[j] != ']' || first)) {
                std::uint32_t lo = p[j];
                if (lo == '\\' && j + 1 < p.size()) {
                    lo = p[++j];
                }
                std::uint32_t hi = lo;
                if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']') {
                    hi = p[j + 2];
                    j += 2;
                }
                g.ranges.emplace_back(lo, hi);
                ++t.count;
                ++j;
                first = false;
            }
            if (j >= p.size()) {
                g.ranges.resize(t.ranges);
                g.tokens.push_back({Tok::Literal, '['});
                continue;
            }
            g.tokens.push_back(t);
            i = j;
        } else {
            g.tokens.push_back({Tok::Literal, c});
        }
    }
    return g;
}

bool inClass(const Glob& g, const Token& t, std::uint32_t c) {
    bool hit = false;
    for (std::uint32_t k = 0; k < t.count && !hit; ++k) {
        const auto& [lo, hi] = g.ranges[t.ranges + k];
        hit = c >= lo && c <= hi;
    }
    return hit != t.negated;
}

// One pass per token over the text: `reach[j]` says the tokens so far can match the first j scalars.
bool matches(const Glob& g, const std::vector<std::uint32_t>& text, std::uint32_t sep) {
    std::size_t n = text.size();
    std::vector<char> reach(n + 1, 0);
    std::vector<char> next(n + 1, 0);
    reach[0] = 1;
    for (const Token& t : g.tokens) {
        std::fill(next.begin(), next.end(), 0);
        bool any = false;
        for (std::size_t j = 0; j <= n; ++j) {
            switch (t.kind) {
            case Tok::Literal:
                next[j] = j > 0 && reach[j - 1] && text[j - 1] == t.c;
                break;
            case Tok::One:
                next[j] = j > 0 && reach[j - 1] && text[j - 1] != sep;
                break;
            case Tok::Class:
                next[j] = j > 0 && reach[j - 1] && text[j - 1] != sep && inClass(g, t, text[j - 1]);
                break;
            case Tok::Star:
                next[j] = reach[j] || (j > 0 && next[j - 1] && text[j - 1] != sep);
                break;
            case Tok::Deep:
                next[j] = reach[j] || (j > 0 && next[j - 1]);
                break;
            case Tok::DeepSep:
                next[j] = reach[j] || (any && text[j - 1] == sep);
                any = any || reach[j];
                break;
            }
        }
        std::swap(reach, next);
    }
    return reach[n] != 0;
}

bool globMatch(vm::HostCall& call, void*) {
    auto pattern = scalars(strArg(call.args[0]));
    auto text = scalars(strArg(call.args[1]));
    auto sepText = scalars(strArg(call.args[2]));
    if (sepText.size() > 1) {
        return misuse(call, "the glob separator is one character or empty");
    }
    std::uint32_t sep = sepText.empty() ? kNoSep : sepText[0];
    std::size_t work = (pattern.size() + 1) * (text.size() + 1);
    if (!call.charge(static_cast<std::int64_t>(work / 8 + (pattern.size() + text.size()) / 4 + 1))) {
        return false;
    }
    call.result.u = matches(compile(pattern, sep), text, sep) ? 1 : 0;
    return true;
}

bool levenshtein(vm::HostCall& call, void*) {
    auto a = scalars(strArg(call.args[0]));
    auto b = scalars(strArg(call.args[1]));
    if (a.size() < b.size()) {
        std::swap(a, b);
    }
    if (!call.charge(static_cast<std::int64_t>(a.size() * b.size() / 4 + (a.size() + b.size()) / 4 + 1))) {
        return false;
    }
    std::vector<std::uint64_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::uint64_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            std::uint64_t above = row[j];
            row[j] = std::min({above + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    call.result.u = row[b.size()];
    return true;
}

} // namespace

bool formatMessage(vm::HostCall& call, void* user);
bool pluralCategory(vm::HostCall& call, void* user);

void addTextNatives(std::vector<Native>& out) {
    out.insert(out.end(), {
                              {"org.quartzlang.text.globMatch", globMatch},
                              {"org.quartzlang.text.levenshtein", levenshtein},
                              {"org.quartzlang.text.formatMessage", formatMessage},
                              {"org.quartzlang.text.pluralCategory", pluralCategory},
                          });
}

} // namespace quartz::stdlib
