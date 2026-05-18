#include "StdFuzz.hpp"
#include "std/regex/Ast.hpp"

#include <algorithm>
#include <functional>

namespace {

using namespace quartz::stdlib::regex;

constexpr const char* kProgram = R"(import org.quartzlang.regex.*
fn run(input: str) -> int {
    let parts = input.split("\u{1}")
    if parts.len() < 2 {
        return 0
    }
    var n = 0
    match compileRegex(parts[0]) {
        Ok(r) => {
            let text = parts[1]
            let all = r.findAll(text)
            if let m = r.find(text) {
                assert(all.len() > 0 && all[0].start == m.start && all[0].end == m.end, "find is the first of findAll")
                assert(text.slice(m.start, m.end) == m.text, "match text")
                for i in 0..r.groupCount() + 1 {
                    n += (m.group(i) ?? "").len()
                }
            } else {
                assert(all.len() == 0, "findAll without find")
            }
            assert(r.isMatch(text) == (all.len() > 0), "isMatch agrees with find")
            n += r.split(text).len() + r.split(text, 2).len()
            let replacement = if parts.len() > 2 { parts[2] } else { "<$0>" }
            if !replacement.contains("$") {
                n += r.replaceAll(text, replacement).len() + r.replace(text, replacement).len()
            }
            n += if r.fullMatch(text) { 1 } else { 0 }
        }
        Err(e) => n += e.len()
    }
    return n
}
)";

/// A backtracking matcher over the parsed pattern with Perl's rules, for tiny inputs only.
class Reference {
public:
    Reference(const Ast& ast, std::string_view text) : ast_(ast), text_(text), caps_(2 * ast.names.size(), kNoPos) {}

    // Leftmost match from `from`; false when there is none or the step budget ran out (then `exhausted` is set).
    bool find(std::uint32_t from, bool anchored, bool fullOnly, bool nonEmptyAtFrom, std::vector<std::uint32_t>& out) {
        for (std::size_t s = from; s <= text_.size();) {
            std::fill(caps_.begin(), caps_.end(), kNoPos);
            caps_[0] = static_cast<std::uint32_t>(s);
            bool ok = match(ast_.root, s, [&](std::size_t j) {
                if ((fullOnly && j != text_.size()) || (nonEmptyAtFrom && s == from && j == from)) {
                    return false;
                }
                caps_[1] = static_cast<std::uint32_t>(j);
                return true;
            });
            if (exhausted) {
                return false;
            }
            if (ok) {
                out = caps_;
                return true;
            }
            if (anchored || s == text_.size()) {
                break;
            }
            decode(text_, s);
        }
        return false;
    }

    bool exhausted = false;

private:
    using Cont = std::function<bool(std::size_t)>;

    bool holds(Assert a, std::size_t i) const {
        auto word = [&](std::size_t k) {
            char c = text_[k];
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        };
        bool before = i > 0 && word(i - 1);
        bool after = i < text_.size() && word(i);
        switch (a) {
        case Assert::Begin:
            return i == 0;
        case Assert::End:
            return i == text_.size();
        case Assert::BeginLine:
            return i == 0 || text_[i - 1] == '\n';
        case Assert::EndLine:
            return i == text_.size() || text_[i] == '\n';
        case Assert::Word:
            return before != after;
        case Assert::NotWord:
            return before == after;
        }
        return false;
    }

    bool match(std::uint32_t id, std::size_t i, const Cont& k) {
        if (++steps_ > 200'000) {
            exhausted = true;
        }
        if (exhausted) {
            return false;
        }
        const Node& n = ast_.nodes[id];
        switch (n.kind) {
        case NodeKind::Empty:
            return k(i);
        case NodeKind::Set:
        case NodeKind::Any:
        case NodeKind::AnyNotNl: {
            if (i >= text_.size()) {
                return false;
            }
            std::size_t j = i;
            std::uint32_t c = decode(text_, j);
            bool in = n.kind == NodeKind::Any || (n.kind == NodeKind::AnyNotNl && c != '\n');
            for (const Range& r : n.kind == NodeKind::Set ? ast_.sets[n.set] : std::vector<Range>{}) {
                in = in || (c >= r.lo && c <= r.hi);
            }
            return in && k(j);
        }
        case NodeKind::Assert:
            return holds(n.assertion, i) && k(i);
        case NodeKind::Group: {
            std::uint32_t a = 2 * n.capture;
            std::uint32_t oldStart = caps_[a];
            std::uint32_t oldEnd = caps_[a + 1];
            caps_[a] = static_cast<std::uint32_t>(i);
            bool ok = match(n.kids[0], i, [&](std::size_t j) {
                std::uint32_t inner = caps_[a + 1];
                caps_[a + 1] = static_cast<std::uint32_t>(j);
                if (k(j)) {
                    return true;
                }
                caps_[a + 1] = inner;
                return false;
            });
            if (!ok) {
                caps_[a] = oldStart;
                caps_[a + 1] = oldEnd;
            }
            return ok;
        }
        case NodeKind::Concat:
            return concat(n, 0, i, k);
        case NodeKind::Alternate:
            for (std::uint32_t kid : n.kids) {
                if (match(kid, i, k)) {
                    return true;
                }
            }
            return false;
        case NodeKind::Repeat:
            return repeat(n, 0, i, k);
        }
        return false;
    }

    bool concat(const Node& n, std::size_t at, std::size_t i, const Cont& k) {
        if (at == n.kids.size()) {
            return k(i);
        }
        return match(n.kids[at], i, [&](std::size_t j) { return concat(n, at + 1, j, k); });
    }

    // Past the minimum, an iteration that matched empty ends the loop, as in Perl.
    bool repeat(const Node& n, std::uint32_t count, std::size_t i, const Cont& k) {
        if (count < n.min) {
            return match(n.kids[0], i, [&](std::size_t j) { return repeat(n, count + 1, j, k); });
        }
        if (n.max != kNoPos && count >= n.max) {
            return k(i);
        }
        auto more = [&] {
            return match(n.kids[0], i,
                         [&](std::size_t j) { return j == i && n.max == kNoPos ? k(j) : repeat(n, count + 1, j, k); });
        };
        return n.greedy ? (more() || k(i)) : (k(i) || more());
    }

    const Ast& ast_;
    std::string_view text_;
    std::vector<std::uint32_t> caps_;
    std::uint64_t steps_ = 0;
};

bool nullable(const Ast& ast, std::uint32_t id) {
    const Node& n = ast.nodes[id];
    switch (n.kind) {
    case NodeKind::Empty:
    case NodeKind::Assert:
        return true;
    case NodeKind::Group:
        return nullable(ast, n.kids[0]);
    case NodeKind::Concat:
        return std::all_of(n.kids.begin(), n.kids.end(), [&](std::uint32_t k) { return nullable(ast, k); });
    case NodeKind::Alternate:
        return std::any_of(n.kids.begin(), n.kids.end(), [&](std::uint32_t k) { return nullable(ast, k); });
    case NodeKind::Repeat:
        return n.min == 0 || nullable(ast, n.kids[0]);
    default:
        return false;
    }
}

// An optional iteration that can match empty is where Perl's captures and the Pike VM's (RE2's) may differ.
bool capturesComparable(const Ast& ast) {
    for (std::uint32_t id = 0; id < ast.nodes.size(); ++id) {
        const Node& n = ast.nodes[id];
        if (n.kind == NodeKind::Repeat && n.max != n.min && nullable(ast, n.kids[0])) {
            return false;
        }
    }
    return true;
}

[[noreturn]] void mismatch(const char* what, std::string_view pattern, std::string_view text) {
    std::fprintf(stderr, "%s differs for pattern '%.*s' and text '%.*s'\n", what, static_cast<int>(pattern.size()),
                 pattern.data(), static_cast<int>(text.size()), text.data());
    std::abort();
}

void compare(std::string_view pattern, std::string_view text) {
    auto ast = parse(pattern);
    auto prog = compile(pattern);
    if (!ast && prog) {
        mismatch("parse and compile", pattern, text);
    }
    if (!prog || pattern.size() > 24 || text.size() > 24) {
        return;
    }
    Matcher m(*prog);
    Meter meter;
    Reference ref(*ast, text);
    std::vector<std::uint32_t> want;
    std::vector<std::uint32_t> got(2 * prog->groups(), kNoPos);
    bool refFound = ref.find(0, false, false, false, want);
    bool found = m.search(text, {.slots = static_cast<std::uint32_t>(got.size())}, got, meter) == Found::Yes;
    if (ref.exhausted) {
        return;
    }
    if (found != refFound ||
        (found && (capturesComparable(*ast) ? got != want : got[0] != want[0] || got[1] != want[1]))) {
        mismatch("find", pattern, text);
    }
    std::vector<std::uint32_t> none;
    bool full =
        m.search(text, {.slots = 0, .anchored = true, .fullOnly = true, .earliest = true}, none, meter) == Found::Yes;
    bool refFull = ref.find(0, true, true, false, want);
    if (!ref.exhausted && full != refFull) {
        mismatch("fullMatch", pattern, text);
    }
    std::uint32_t from = 0;
    bool lastEmpty = false;
    for (int n = 0; n < 64 && !ref.exhausted; ++n) {
        got.assign(2, kNoPos);
        found = m.search(text, {.from = from, .slots = 2, .nonEmptyAtFrom = lastEmpty}, got, meter) == Found::Yes;
        refFound = ref.find(from, false, false, lastEmpty, want);
        if (ref.exhausted) {
            return;
        }
        if (found != refFound || (found && (got[0] != want[0] || got[1] != want[1]))) {
            mismatch("findAll", pattern, text);
        }
        if (!found) {
            break;
        }
        lastEmpty = got[0] == got[1];
        from = got[1];
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    std::size_t cut = input.find('\x01');
    if (cut != std::string_view::npos) {
        std::string_view rest = input.substr(cut + 1);
        compare(input.substr(0, cut), rest.substr(0, rest.find('\x01')));
    }
    return quartz::fuzz::runStd(kProgram, data, size, true);
}
