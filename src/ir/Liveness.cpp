#include "ir/Liveness.hpp"

#include <array>

namespace quartz::ir {

namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFF;

void setBit(std::uint64_t* row, Reg r) { row[r >> 6] |= std::uint64_t{1} << (r & 63); }

} // namespace

Liveness::Liveness(const Function& fn) : fn_(fn) {
    const std::size_t n = fn.code.size();
    const std::size_t regs = fn.regs.size();
    words_ = (regs + 63) / 64;
    std::vector<std::size_t> endOf(n, n);
    std::vector<std::size_t> elseOf(n, n);
    std::vector<std::size_t> open;
    for (std::size_t pc = 0; pc < n; ++pc) {
        Op op = fn.code[pc].op;
        if (op == Op::Block || op == Op::Loop || op == Op::If) {
            open.push_back(pc);
        } else if (op == Op::Else && !open.empty()) {
            elseOf[open.back()] = pc;
        } else if (op == Op::End && !open.empty()) {
            endOf[open.back()] = pc;
            open.pop_back();
        }
    }
    auto next = [&](std::size_t pc) { return pc + 1 < n ? static_cast<std::uint32_t>(pc + 1) : kNone; };
    auto target = [&](std::uint32_t depth) -> std::uint32_t {
        if (depth >= open.size()) {
            return kNone;
        }
        std::size_t f = open[open.size() - 1 - depth];
        return fn.code[f].op == Op::Loop ? next(f) : static_cast<std::uint32_t>(endOf[f] < n ? endOf[f] : kNone);
    };
    std::vector<std::array<std::uint32_t, 2>> succ(n, {kNone, kNone});
    std::vector<std::uint64_t> use(n * words_, 0);
    std::vector<std::uint64_t> def(n * words_, 0);
    open.clear();
    for (std::size_t pc = 0; pc < n; ++pc) {
        const Inst& in = fn.code[pc];
        std::uint64_t* u = use.data() + pc * words_;
        std::uint64_t* d = def.data() + pc * words_;
        auto addUse = [&](Reg r) {
            if (r < regs) {
                setBit(u, r);
            }
        };
        switch (in.op) {
        case Op::Block:
        case Op::Loop:
            succ[pc] = {next(pc), kNone};
            open.push_back(pc);
            break;
        case Op::If:
            succ[pc] = {next(pc), elseOf[pc] < n ? next(elseOf[pc]) : static_cast<std::uint32_t>(endOf[pc])};
            open.push_back(pc);
            break;
        case Op::Else:
            succ[pc] = {open.empty() ? kNone : static_cast<std::uint32_t>(endOf[open.back()]), kNone};
            break;
        case Op::End:
            succ[pc] = {next(pc), kNone};
            if (!open.empty()) {
                open.pop_back();
            }
            break;
        case Op::Br:
            succ[pc] = {target(in.imm), kNone};
            break;
        case Op::BrIf:
            succ[pc] = {next(pc), target(in.imm)};
            break;
        case Op::Return:
        case Op::Trap:
            break;
        default:
            succ[pc] = {next(pc), kNone};
            break;
        }
        for (std::uint32_t i = 0; i < in.argCount && in.argBegin + i < fn.args.size(); ++i) {
            addUse(fn.args[in.argBegin + i]);
        }
        if (in.op == Op::ScopeExit) {
            for (Reg r = in.a; r < in.b && r < regs; ++r) {
                setBit(d, r);
            }
            continue;
        }
        if (in.a != kNoReg) {
            addUse(in.a);
        }
        if (in.b != kNoReg && in.op != Op::EnumField) {
            addUse(in.b);
        }
        if (in.dst != kNoReg && in.dst < regs) {
            setBit(d, in.dst);
        }
    }
    in_ = use;
    out_.assign(n * words_, 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t pc = n; pc-- > 0;) {
            std::uint64_t* o = out_.data() + pc * words_;
            for (std::uint32_t s : succ[pc]) {
                if (s == kNone) {
                    continue;
                }
                const std::uint64_t* si = in_.data() + static_cast<std::size_t>(s) * words_;
                for (std::size_t w = 0; w < words_; ++w) {
                    o[w] |= si[w];
                }
            }
            std::uint64_t* i = in_.data() + pc * words_;
            const std::uint64_t* u = use.data() + pc * words_;
            const std::uint64_t* d = def.data() + pc * words_;
            for (std::size_t w = 0; w < words_; ++w) {
                std::uint64_t v = u[w] | (o[w] & ~d[w]);
                if (v != i[w]) {
                    i[w] = v;
                    changed = true;
                }
            }
        }
    }
}

std::vector<Reg> Liveness::set(const std::vector<std::uint64_t>& bits, std::size_t pc) const {
    std::vector<Reg> out;
    if (pc >= fn_.code.size()) {
        return out;
    }
    const std::uint64_t* row = bits.data() + pc * words_;
    for (std::size_t w = 0; w < words_; ++w) {
        for (std::uint64_t v = row[w]; v != 0; v &= v - 1) {
            out.push_back(static_cast<Reg>(w * 64 + static_cast<std::size_t>(__builtin_ctzll(v))));
        }
    }
    return out;
}

std::vector<Reg> Liveness::kept(std::size_t pc, ResumeAt at) const {
    if (at == ResumeAt::BeforeCall) {
        return set(in_, pc);
    }
    std::vector<Reg> out = set(out_, pc);
    if (at == ResumeAt::After && pc < fn_.code.size()) {
        Reg dst = fn_.code[pc].dst;
        std::erase(out, dst);
    }
    return out;
}

} // namespace quartz::ir
