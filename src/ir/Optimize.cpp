#include "ir/Optimize.hpp"

#include <algorithm>

namespace quartz::ir {

namespace {

struct LoopSpan {
    std::size_t begin; // index of the Loop instruction
    std::size_t end;   // index of its End
};

std::vector<LoopSpan> findLoops(const Function& fn) {
    std::vector<LoopSpan> loops;
    std::vector<std::pair<Op, std::size_t>> stack;
    for (std::size_t i = 0; i < fn.code.size(); ++i) {
        Op op = fn.code[i].op;
        if (op == Op::Block || op == Op::Loop || op == Op::If) {
            stack.emplace_back(op, i);
        } else if (op == Op::End && !stack.empty()) {
            if (stack.back().first == Op::Loop) {
                loops.push_back({stack.back().second, i});
            }
            stack.pop_back();
        }
    }
    // Innermost loops first, so an outer pass can hoist further.
    std::sort(loops.begin(), loops.end(), [](const LoopSpan& a, const LoopSpan& b) { return a.end - a.begin < b.end - b.begin; });
    return loops;
}

// A state field read moves out of a loop when nothing in the loop can change that field: no FieldSet on a state of
// the same type and no call that receives such a state (only code given a state can modify it).
bool hoistStateReads(Module& m, Function& fn) {
    const TypeTable& t = m.types;
    std::vector<std::uint32_t> defs(fn.regs.size(), 0);
    for (const Inst& in : fn.code) {
        if (in.dst != kNoReg) {
            ++defs[in.dst];
        }
        if (in.op == Op::FieldSet) {
            ++defs[in.a];
        }
    }
    for (const LoopSpan& loop : findLoops(fn)) {
        std::vector<std::pair<TypeId, std::uint32_t>> written;
        std::vector<TypeId> passed;
        std::vector<bool> definedInside(fn.regs.size(), false);
        bool suspends = false;
        for (std::size_t i = loop.begin + 1; i < loop.end; ++i) {
            const Inst& in = fn.code[i];
            // A view held across a suspension becomes a task-local copy (D35), so it must be read after each resume.
            suspends = suspends || in.op == Op::Wait || (in.op == Op::Call && m.functions[in.imm].isSuspend);
            if (in.dst != kNoReg) {
                definedInside[in.dst] = true;
            }
            if (in.op == Op::FieldSet && t.kind(fn.regs[in.a]) == TypeKind::State) {
                written.emplace_back(fn.regs[in.a], in.imm);
            }
            if (in.op == Op::Call) {
                for (std::uint32_t k = 0; k < in.argCount; ++k) {
                    TypeId at = fn.regs[fn.args[in.argBegin + k]];
                    if (t.kind(at) == TypeKind::State) {
                        passed.push_back(at);
                    }
                }
            }
        }
        if (suspends) {
            continue;
        }
        std::vector<Inst> hoisted;
        std::vector<Inst> kept;
        for (std::size_t i = loop.begin + 1; i < loop.end; ++i) {
            const Inst& in = fn.code[i];
            bool candidate = in.op == Op::FieldGet && t.kind(fn.regs[in.a]) == TypeKind::State &&
                             !definedInside[in.a] && defs[in.dst] == 1;
            if (candidate) {
                TypeId st = fn.regs[in.a];
                candidate = std::find(passed.begin(), passed.end(), st) == passed.end() &&
                            std::find(written.begin(), written.end(), std::make_pair(st, in.imm)) == written.end();
            }
            (candidate ? hoisted : kept).push_back(in);
        }
        if (hoisted.empty()) {
            continue;
        }
        std::vector<Inst> code;
        code.reserve(fn.code.size());
        code.insert(code.end(), fn.code.begin(), fn.code.begin() + static_cast<std::ptrdiff_t>(loop.begin));
        code.insert(code.end(), hoisted.begin(), hoisted.end());
        code.push_back(fn.code[loop.begin]);
        code.insert(code.end(), kept.begin(), kept.end());
        code.insert(code.end(), fn.code.begin() + static_cast<std::ptrdiff_t>(loop.end), fn.code.end());
        fn.code = std::move(code);
        return true; // indices changed; the caller runs the pass again
    }
    return false;
}

} // namespace

void optimize(Module& m) {
    for (Function& fn : m.functions) {
        for (int round = 0; round < 64 && hoistStateReads(m, fn); ++round) {
        }
    }
}

} // namespace quartz::ir
