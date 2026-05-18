#include "std/regex/Ast.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <optional>

namespace quartz::stdlib::regex {

bool CharSet::has(std::uint32_t c) const {
    if (c < 128) {
        return (ascii[c >> 6] >> (c & 63) & 1) != 0;
    }
    auto it = std::upper_bound(wide.begin(), wide.end(), c, [](std::uint32_t v, const Range& r) { return v < r.lo; });
    return it != wide.begin() && c <= std::prev(it)->hi;
}

std::size_t Program::footprint() const {
    std::size_t n = insts.size();
    std::size_t bytes = sizeof(Program) + n * sizeof(Inst) + prefix.size();
    for (const CharSet& s : sets) {
        bytes += sizeof(CharSet) + s.wide.size() * sizeof(Range);
    }
    for (const std::string& name : names) {
        bytes += sizeof(std::string) + name.size();
    }
    std::size_t slots = 2 * static_cast<std::size_t>(groups());
    // Matcher: two lists (dense, sparse, captures), the closure stack and two capture rows.
    return bytes + 2 * n * (2 * sizeof(std::uint32_t) + slots * sizeof(std::uint32_t)) + 2 * n * 12 + 2 * slots * 4;
}

namespace {

std::uint8_t leadByte(std::uint32_t cp) {
    return static_cast<std::uint8_t>(cp < 0x80      ? cp
                                     : cp < 0x800   ? 0xC0 | (cp >> 6)
                                     : cp < 0x10000 ? 0xE0 | (cp >> 12)
                                                    : 0xF0 | (cp >> 18));
}

void appendUtf8(std::string& out, std::uint32_t cp) {
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
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Compiler {
public:
    Compiler(const Ast& ast, Program& prog) : ast_(ast), prog_(prog), setOf_(ast.sets.size(), kNoPos) {}

    bool emit(std::uint32_t id) {
        const Node& n = ast_.nodes[id];
        switch (n.kind) {
        case NodeKind::Empty:
            return true;
        case NodeKind::Set:
            return emitSet(n);
        case NodeKind::Any:
            return push({Op::Any}, n) != kNoPos;
        case NodeKind::AnyNotNl:
            return push({Op::AnyNotNl}, n) != kNoPos;
        case NodeKind::Assert:
            return push({.op = Op::Assert, .assertion = n.assertion}, n) != kNoPos;
        case NodeKind::Group:
            return push({.op = Op::Save, .x = 2 * n.capture}, n) != kNoPos && emit(n.kids[0]) &&
                   push({.op = Op::Save, .x = 2 * n.capture + 1}, n) != kNoPos;
        case NodeKind::Concat:
            return std::all_of(n.kids.begin(), n.kids.end(), [&](std::uint32_t k) { return emit(k); });
        case NodeKind::Alternate:
            return emitAlternate(n);
        case NodeKind::Repeat:
            return emitRepeat(n);
        }
        return false;
    }

    std::uint32_t push(Inst in, const Node& at) {
        if (prog_.insts.size() >= kMaxInsts) {
            error = Error{std::format("pattern too large (more than {} instructions)", kMaxInsts), at.pos};
            return kNoPos;
        }
        prog_.insts.push_back(in);
        return static_cast<std::uint32_t>(prog_.insts.size() - 1);
    }

    std::optional<Error> error;

private:
    [[nodiscard]] std::uint32_t here() const { return static_cast<std::uint32_t>(prog_.insts.size()); }

    void setSplit(std::uint32_t pc, std::uint32_t body, std::uint32_t exit, bool greedy) {
        prog_.insts[pc].x = greedy ? body : exit;
        prog_.insts[pc].y = greedy ? exit : body;
    }

    bool emitSet(const Node& n) {
        const std::vector<Range>& r = ast_.sets[n.set];
        Inst in{Op::Set};
        if (r.size() == 1) {
            bool all = r[0].lo == 0 && r[0].hi == kMaxCodePoint;
            in = all ? Inst{Op::Any} : Inst{.op = Op::Char, .x = r[0].lo, .y = r[0].hi};
        } else if (r.size() == 2 && r[0].lo == 0 && r[0].hi == '\n' - 1 && r[1].lo == '\n' + 1 &&
                   r[1].hi == kMaxCodePoint) {
            in = Inst{Op::AnyNotNl};
        } else {
            in.x = setIndex(n.set);
        }
        return push(in, n) != kNoPos;
    }

    std::uint32_t setIndex(std::uint32_t astSet) {
        if (setOf_[astSet] == kNoPos) {
            CharSet s;
            for (const Range& r : ast_.sets[astSet]) {
                for (std::uint32_t c = r.lo; c <= std::min<std::uint32_t>(r.hi, 127); ++c) {
                    s.ascii[c >> 6] |= std::uint64_t{1} << (c & 63);
                }
                if (r.hi >= 128) {
                    s.wide.push_back({std::max<std::uint32_t>(r.lo, 128), r.hi});
                }
            }
            prog_.sets.push_back(std::move(s));
            setOf_[astSet] = static_cast<std::uint32_t>(prog_.sets.size() - 1);
        }
        return setOf_[astSet];
    }

    bool emitAlternate(const Node& n) {
        std::vector<std::uint32_t> jumps;
        for (std::size_t k = 0; k < n.kids.size(); ++k) {
            bool last = k + 1 == n.kids.size();
            std::uint32_t split = last ? kNoPos : push({Op::Split}, n);
            if ((!last && split == kNoPos) || !emit(n.kids[k])) {
                return false;
            }
            if (!last) {
                std::uint32_t jump = push({Op::Jmp}, n);
                if (jump == kNoPos) {
                    return false;
                }
                jumps.push_back(jump);
                setSplit(split, split + 1, here(), true);
            }
        }
        for (std::uint32_t j : jumps) {
            prog_.insts[j].x = here();
        }
        return true;
    }

    // x* compiles as (x+)?, so an iteration that matches empty keeps its captures, as in Perl and RE2.
    bool emitRepeat(const Node& n) {
        std::uint32_t kid = n.kids[0];
        if (n.max == kNoPos) {
            std::uint32_t entry = kNoPos;
            if (n.min == 0) {
                entry = push({Op::Split}, n);
                if (entry == kNoPos) {
                    return false;
                }
            }
            for (std::uint32_t k = 1; k < n.min; ++k) {
                if (!emit(kid)) {
                    return false;
                }
            }
            std::uint32_t body = here();
            std::uint32_t loop = kNoPos;
            if (!emit(kid) || (loop = push({Op::Split}, n)) == kNoPos) {
                return false;
            }
            setSplit(loop, body, here(), n.greedy);
            if (entry != kNoPos) {
                setSplit(entry, body, here(), n.greedy);
            }
            return true;
        }
        for (std::uint32_t k = 0; k < n.min; ++k) {
            if (!emit(kid)) {
                return false;
            }
        }
        std::vector<std::uint32_t> splits;
        for (std::uint32_t k = n.min; k < n.max; ++k) {
            std::uint32_t split = push({Op::Split}, n);
            if (split == kNoPos || !emit(kid)) {
                return false;
            }
            splits.push_back(split);
        }
        for (std::uint32_t s : splits) {
            setSplit(s, s + 1, here(), n.greedy);
        }
        return true;
    }

    const Ast& ast_;
    Program& prog_;
    std::vector<std::uint32_t> setOf_;
};

bool consumes(Op op) { return op == Op::Char || op == Op::Set || op == Op::Any || op == Op::AnyNotNl; }

// Every path from the start crosses \A (or ^ outside multi-line mode) before it reads or matches.
bool startsAnchored(const Program& p) {
    std::vector<char> seen(p.insts.size(), 0);
    std::vector<std::uint32_t> todo{0};
    while (!todo.empty()) {
        std::uint32_t pc = todo.back();
        todo.pop_back();
        if (seen[pc]) {
            continue;
        }
        seen[pc] = 1;
        const Inst& in = p.insts[pc];
        if (consumes(in.op) || in.op == Op::Match) {
            return false;
        }
        if (in.op == Op::Assert && in.assertion == Assert::Begin) {
            continue;
        }
        if (in.op == Op::Split) {
            todo.push_back(in.y);
        }
        todo.push_back(in.op == Op::Jmp || in.op == Op::Split ? in.x : pc + 1);
    }
    return true;
}

void addLeads(std::uint64_t* bytes, std::uint32_t lo, std::uint32_t hi) {
    auto mark = [&](std::uint32_t b) { bytes[b >> 6] |= std::uint64_t{1} << (b & 63); };
    for (std::uint32_t b = lo; b <= std::min<std::uint32_t>(hi, 0x7F); ++b) {
        mark(b);
    }
    if (hi >= 0x80) {
        for (std::uint32_t b = leadByte(std::max<std::uint32_t>(lo, 0x80)); b <= leadByte(hi); ++b) {
            mark(b);
        }
    }
    // A malformed byte reads as U+FFFD.
    if (lo <= 0xFFFD && hi >= 0xFFFD) {
        for (std::uint32_t b = 0x80; b < 256; ++b) {
            mark(b);
        }
    }
}

// The bytes a match can start with; false when the start can match empty or depends on context.
bool firstBytes(const Program& p, std::uint64_t* bytes) {
    std::vector<char> seen(p.insts.size(), 0);
    std::vector<std::uint32_t> todo{0};
    while (!todo.empty()) {
        std::uint32_t pc = todo.back();
        todo.pop_back();
        if (seen[pc]) {
            continue;
        }
        seen[pc] = 1;
        const Inst& in = p.insts[pc];
        switch (in.op) {
        case Op::Match:
        case Op::Assert:
            return false;
        case Op::Split:
            todo.push_back(in.y);
            todo.push_back(in.x);
            break;
        case Op::Jmp:
            todo.push_back(in.x);
            break;
        case Op::Save:
            todo.push_back(pc + 1);
            break;
        case Op::Char:
            addLeads(bytes, in.x, in.y);
            break;
        case Op::Any:
        case Op::AnyNotNl:
            addLeads(bytes, 0, kMaxCodePoint);
            break;
        case Op::Set: {
            const CharSet& s = p.sets[in.x];
            bytes[0] |= s.ascii[0];
            bytes[1] |= s.ascii[1];
            for (const Range& r : s.wide) {
                addLeads(bytes, r.lo, r.hi);
            }
            break;
        }
        }
    }
    int count = 0;
    for (int k = 0; k < 4; ++k) {
        count += std::popcount(bytes[k]);
    }
    return count <= 128;
}

std::string literalPrefix(const Program& p) {
    std::string out;
    for (std::uint32_t pc = 0; pc < p.insts.size() && out.size() < 64; ++pc) {
        const Inst& in = p.insts[pc];
        if (in.op == Op::Char && in.x == in.y) {
            appendUtf8(out, in.x);
        } else if (in.op != Op::Save) {
            break;
        }
    }
    return out;
}

} // namespace

std::expected<Program, Error> compile(std::string_view pattern) {
    auto ast = parse(pattern);
    if (!ast) {
        return std::unexpected(std::move(ast.error()));
    }
    Program prog;
    prog.names = std::move(ast->names);
    std::size_t slots = 2 * prog.names.size();
    Compiler c(*ast, prog);
    Node whole;
    bool ok = c.push({.op = Op::Save, .x = 0}, whole) != kNoPos && c.emit(ast->root) &&
              c.push({.op = Op::Save, .x = 1}, whole) != kNoPos && c.push({Op::Match}, whole) != kNoPos;
    if (!ok) {
        c.error->insts = prog.insts.size();
        return std::unexpected(std::move(*c.error));
    }
    if (prog.insts.size() * slots > kMaxCaptureCells) {
        return std::unexpected(Error{"pattern too large for its number of groups", 0, prog.insts.size()});
    }
    prog.anchoredStart = startsAnchored(prog);
    prog.useFirstBytes = firstBytes(prog, prog.firstBytes);
    prog.prefix = literalPrefix(prog);
    return prog;
}

} // namespace quartz::stdlib::regex
