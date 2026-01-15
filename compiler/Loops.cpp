#include "Codegen.hpp"

#include <limits>

namespace quartz::compiler {

using ir::Op;

namespace {

bool isI64Compare(const ir::Inst& in) { return in.op == Op::Lt || in.op == Op::Le; }

} // namespace

std::size_t FunctionCodegen::matchingEnd(std::size_t loop) const {
    std::size_t depth = 0;
    for (std::size_t k = loop; k < fn_.code.size(); ++k) {
        Op op = fn_.code[k].op;
        if (op == Op::Block || op == Op::Loop || op == Op::If) {
            ++depth;
        } else if (op == Op::End && --depth == 0) {
            return k;
        }
    }
    return fn_.code.size();
}

bool FunctionCodegen::fallsThroughOnly(std::size_t end) const {
    std::size_t depth = 0;
    for (std::size_t k = end; k-- > 0;) {
        const ir::Inst& in = fn_.code[k];
        if (in.op == Op::End) {
            ++depth;
        } else if (in.op == Op::Block || in.op == Op::Loop || in.op == Op::If) {
            if (depth == 0) {
                return in.op == Op::Block;
            }
            --depth;
        } else if ((in.op == Op::Br || in.op == Op::BrIf) && in.imm == depth) {
            return false;
        }
    }
    return false;
}

// A loop whose only fuel charge is its own back edge, driven by `i += s` (s >= 1) against a bound that does not
// change inside it. Then hi - i0 + 1 bounds the back edges, and with that much fuel left no charge can trap.
bool FunctionCodegen::countedLoop(std::size_t loop, CountedLoop& out) const {
    const std::vector<ir::Inst>& code = fn_.code;
    std::size_t end = matchingEnd(loop);
    if (end >= code.size() || end < loop + 4) {
        return false;
    }
    std::size_t k = end - 1;
    std::size_t tail = 0;
    bool listBound = false;
    if (code[k].op == Op::BrIf && code[k].imm == 0 && isI64Compare(code[k - 1]) && code[k - 1].dst == code[k].a) {
        const ir::Inst& cmp = code[k - 1];
        out.i = cmp.a;
        out.hi = cmp.b;
        k -= 2;
        if (code[k].op == Op::Intrinsic && static_cast<ir::Intrinsic>(code[k].imm) == ir::Intrinsic::ListLen &&
            code[k].dst == cmp.b) {
            listBound = true;
            out.hi = fn_.args[code[k].argBegin];
            --k;
        }
        tail = k;
    } else if (code[k].op == Op::Br && code[k].imm == 0) {
        tail = k - 1;
    } else {
        return false;
    }
    // `for` bodies sit in a block; its end is only reached by falling through when nothing branches to it.
    if (code[tail].op == Op::End && !fallsThroughOnly(tail)) {
        return false;
    }
    if (code[tail].op == Op::End) {
        --tail;
    }
    // The increment: `i = i + s`, or `t = i + s; i = t`.
    std::size_t addAt = tail;
    if (code[tail].op == Op::Move && code[tail].dst != ir::kNoReg && tail > loop + 1) {
        addAt = tail - 1;
        const ir::Inst& add = code[addAt];
        if (add.op != Op::Add || add.dst != code[tail].a || code[tail].dst != add.a) {
            return false;
        }
    } else if (code[tail].op != Op::Add || code[tail].dst != code[tail].a) {
        return false;
    }
    const ir::Inst& add = code[addAt];
    ir::Reg iv = add.a;
    out.step = add.b;
    if (code[end - 1].op == Op::Br) {
        // Inclusive form: `t = i < hi; u = !t; br_if u <exit>; i += s; br loop`.
        if (addAt < loop + 4) {
            return false;
        }
        const ir::Inst& exit = code[addAt - 1];
        const ir::Inst& no = code[addAt - 2];
        const ir::Inst& cmp = code[addAt - 3];
        if (exit.op != Op::BrIf || exit.imm < 1 || no.op != Op::Not || no.dst != exit.a || cmp.op != Op::Lt ||
            cmp.dst != no.a || cmp.a != iv) {
            return false;
        }
        out.i = iv;
        out.hi = cmp.b;
    }
    if (out.i != iv || fn_.regs[iv] != ir::kI64 || fn_.regs[out.step] != ir::kI64 ||
        (!listBound && fn_.regs[out.hi] != ir::kI64)) {
        return false;
    }
    out.listBound = listBound;
    // Nothing else in the loop may charge fuel, write the induction inputs or change the bound list's length.
    std::size_t depth = 0;
    for (std::size_t j = loop + 1; j < end; ++j) {
        const ir::Inst& in = code[j];
        switch (in.op) {
        case Op::Loop:
        case Op::Call:
            return false;
        case Op::Block:
        case Op::If:
            ++depth;
            break;
        case Op::End:
            --depth;
            break;
        case Op::Br:
        case Op::BrIf:
            if (in.imm == depth && j != end - 1) {
                return false;
            }
            break;
        case Op::Intrinsic: {
            auto op = static_cast<ir::Intrinsic>(in.imm);
            if (listBound &&
                (op == ir::Intrinsic::ListPush || op == ir::Intrinsic::ListPop || op == ir::Intrinsic::ListClear)) {
                return false;
            }
            break;
        }
        default:
            break;
        }
        if (in.dst == ir::kNoReg) {
            continue;
        }
        bool increment = j == addAt || (j == tail && tail != addAt);
        if ((in.dst == iv && !increment) || in.dst == out.step || in.dst == out.hi) {
            return false;
        }
    }
    out.end = end;
    return true;
}

void FunctionCodegen::versionLoop(std::size_t loop, const CountedLoop& c) {
    llvm::Value* i0 = read1(c.i);
    llvm::Value* step = read1(c.step);
    llvm::Value* hi = c.listBound ? listLen(read1(c.hi)) : read1(c.hi);
    llvm::Value* fuel = B_.CreateLoad(U.i64, fuel_);
    llvm::Value* max = constI64(std::numeric_limits<std::int64_t>::max());
    llvm::Value* ok = B_.CreateICmpSGE(step, constI64(1));
    ok = B_.CreateAnd(ok, B_.CreateICmpSLE(i0, hi));
    ok = B_.CreateAnd(ok, B_.CreateICmpSLE(hi, B_.CreateSub(max, step)));
    llvm::Value* backEdges = B_.CreateAdd(B_.CreateSub(hi, i0), constI64(1));
    ok = B_.CreateAnd(ok, B_.CreateICmpSGE(fuel, constI64(0)));
    ok = B_.CreateAnd(ok, B_.CreateICmpUGE(fuel, backEdges));
    llvm::BasicBlock* fast = llvm::BasicBlock::Create(U.C, "loop.counted", F_);
    llvm::BasicBlock* slow = llvm::BasicBlock::Create(U.C, "loop", F_);
    llvm::BasicBlock* after = llvm::BasicBlock::Create(U.C, "loop.end", F_);
    B_.CreateCondBr(ok, fast, slow);
    // The checked copy goes first so the unchecked one can jump into it after a host call charged fuel.
    resume_.clear();
    for (llvm::BasicBlock* head : {slow, fast}) {
        checkFuel_ = head == slow;
        recordResume_ = head == slow;
        B_.SetInsertPoint(head);
        frames_.push_back({Op::Loop, head});
        for (std::size_t pc = loop + 1; pc < c.end && error_.empty(); ++pc) {
            line_ = fn_.code[pc].line;
            pc_ = pc;
            lower(fn_.code[pc]);
        }
        frames_.pop_back();
        if (!B_.GetInsertBlock()->hasTerminator()) {
            B_.CreateBr(after);
        }
    }
    checkFuel_ = true;
    recordResume_ = false;
    B_.SetInsertPoint(after);
}

} // namespace quartz::compiler
