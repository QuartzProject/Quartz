#include "Codegen.hpp"
#include "vm/NativeAbi.hpp"

#include <bit>
#include <cstddef>
#include <llvm/IR/Intrinsics.h>

namespace quartz::compiler {

using ir::Op;
using ir::TypeId;
using ir::TypeKind;

namespace {

constexpr auto kAlreadySet = static_cast<ir::TrapCode>(vm::kTrapAlreadySet);

} // namespace

void FunctionCodegen::lower(const ir::Inst& in) {
    switch (in.op) {
    case Op::Block:
        frames_.push_back({Op::Block, llvm::BasicBlock::Create(U.C, "block.end", F_)});
        return;
    case Op::Loop: {
        llvm::BasicBlock* head = llvm::BasicBlock::Create(U.C, "loop", F_);
        B_.CreateBr(head);
        B_.SetInsertPoint(head);
        frames_.push_back({Op::Loop, head, nullptr, pc_});
        return;
    }
    case Op::If: {
        llvm::BasicBlock* then = llvm::BasicBlock::Create(U.C, "then", F_);
        llvm::BasicBlock* other = llvm::BasicBlock::Create(U.C, "else", F_);
        llvm::BasicBlock* end = llvm::BasicBlock::Create(U.C, "if.end", F_);
        B_.CreateCondBr(read1(in.a), then, other);
        B_.SetInsertPoint(then);
        frames_.push_back({Op::If, end, other});
        return;
    }
    case Op::Else: {
        Frame& f = frames_.back();
        if (!B_.GetInsertBlock()->hasTerminator()) {
            B_.CreateBr(f.target);
        }
        B_.SetInsertPoint(f.elseBlock);
        f.elseBlock = nullptr;
        return;
    }
    case Op::End: {
        Frame f = frames_.back();
        frames_.pop_back();
        if (f.op == Op::Loop) {
            return;
        }
        if (!B_.GetInsertBlock()->hasTerminator()) {
            B_.CreateBr(f.target);
        }
        if (f.elseBlock != nullptr) {
            B_.SetInsertPoint(f.elseBlock);
            B_.CreateBr(f.target);
        }
        B_.SetInsertPoint(f.target);
        return;
    }
    case Op::Br:
        branch(in.imm, nullptr);
        deadBlock();
        return;
    case Op::BrIf:
        branch(in.imm, read1(in.a));
        return;
    case Op::Return:
        ret(in);
        deadBlock();
        return;
    case Op::Trap:
        emitTrap(static_cast<ir::TrapCode>(in.imm));
        deadBlock();
        return;
    case Op::Const: {
        const ir::Constant& c = U.IR.constants[in.imm];
        TypeId t = fn_.regs[in.dst];
        llvm::Value* v = nullptr;
        if (c.kind == ir::ConstKind::Str) {
            v = U.stringConstant(in.imm);
        } else if (c.kind == ir::ConstKind::Float) {
            double d = std::bit_cast<double>(c.bits);
            v = t == ir::kF32 ? llvm::ConstantFP::get(U.f32, static_cast<double>(static_cast<float>(d)))
                              : llvm::ConstantFP::get(U.f64, d);
        } else if (t == ir::kBool) {
            v = llvm::ConstantInt::get(U.i1, c.bits & 1);
        } else {
            v = llvm::ConstantInt::get(U.i64, c.bits);
        }
        write1(in.dst, v);
        return;
    }
    case Op::Move:
        write(in.dst, read(in.a));
        return;
    case Op::Neg:
    case Op::Not:
    case Op::BitNot:
        unary(in);
        return;
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::Rem:
    case Op::BitAnd:
    case Op::BitOr:
    case Op::BitXor:
    case Op::Shl:
    case Op::Shr:
        binary(in);
        return;
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
        compare(in);
        return;
    case Op::Convert:
        convert(in);
        return;
    case Op::Call:
        call(in);
        return;
    case Op::CallHost:
        callHost(in);
        return;
    case Op::Closure:
        makeClosure(in);
        return;
    case Op::CallValue:
        callValue(in);
        return;
    case Op::Intrinsic:
        intrinsic(in);
        return;
    case Op::FieldGet:
    case Op::FieldSet:
        field(in);
        return;
    case Op::Make: {
        Values all;
        for (std::uint32_t i = 0; i < in.argCount; ++i) {
            Values v = read(fn_.args[in.argBegin + i]);
            all.insert(all.end(), v.begin(), v.end());
        }
        write(in.dst, all);
        return;
    }
    case Op::IsNull:
    case Op::Unwrap:
    case Op::Wrap:
    case Op::NullOf:
        nullable(in);
        return;
    case Op::EnumMake:
    case Op::EnumTag:
    case Op::EnumField:
        enumOp(in);
        return;
    case Op::ScopeEnter:
    case Op::ScopeExit:
    case Op::Persist:
        scope(in);
        return;
    case Op::Wait:
        wait(in);
        return;
    }
    error_ = "unsupported instruction";
}

void FunctionCodegen::scope(const ir::Inst& in) {
    switch (in.op) {
    case Op::ScopeEnter:
        trapIf(B_.CreateICmpEQ(B_.CreateCall(U.runtime(Rt::ScopeIn), {ctx_}), constI32(0)), kAlreadySet);
        return;
    case Op::ScopeExit:
        B_.CreateCall(U.runtime(Rt::ScopeOut), {ctx_});
        return;
    default: {
        TypeId t = fn_.regs[in.a];
        const std::vector<SlotKind>& k = kindsOf(in.a);
        llvm::Value* s = scratch(static_cast<std::uint32_t>(k.size()));
        storeSlots(s, 0, read(in.a), k, U.tbaaSlot);
        llvm::Value* ok = B_.CreateCall(U.runtime(Rt::Persist), {ctx_, constI32(t), s, constI32(in.imm)});
        trapIf(B_.CreateICmpEQ(ok, constI32(0)), kAlreadySet);
        write(in.a, loadSlots(s, 0, k, U.tbaaSlot));
        return;
    }
    }
}

void FunctionCodegen::branch(std::uint32_t depth, llvm::Value* cond) {
    Frame& f = frames_[frames_.size() - 1 - depth];
    if (f.op != Op::Loop) {
        if (cond == nullptr) {
            B_.CreateBr(f.target);
            return;
        }
        llvm::BasicBlock* cont = llvm::BasicBlock::Create(U.C, "cont", F_);
        B_.CreateCondBr(cond, f.target, cont);
        B_.SetInsertPoint(cont);
        return;
    }
    std::uint32_t state = ir::resumeState(f.pc, ir::ResumeAt::LoopHead);
    // parity: the interpreter reports the line of the instruction before the loop head (Interp.cpp, SHORT_JUMP)
    std::uint32_t line = line_;
    for (const auto& [pc, l] : cf_.loopLines) {
        if (pc == f.pc) {
            line_ = l;
        }
    }
    if (cond == nullptr) {
        chargeFuel(state, f.target);
        line_ = line;
        B_.CreateBr(f.target);
        return;
    }
    llvm::BasicBlock* back = llvm::BasicBlock::Create(U.C, "backedge", F_);
    llvm::BasicBlock* cont = llvm::BasicBlock::Create(U.C, "cont", F_);
    B_.CreateCondBr(cond, back, cont);
    B_.SetInsertPoint(back);
    chargeFuel(state, f.target);
    line_ = line;
    B_.CreateBr(f.target);
    B_.SetInsertPoint(cont);
}

void FunctionCodegen::ret(const ir::Inst& in) {
    llvm::Value* r = llvm::UndefValue::get(U.resultType(index_));
    r = B_.CreateInsertValue(r, llvm::ConstantInt::getFalse(U.C), 0);
    if (in.a != ir::kNoReg) {
        Values v = read(in.a);
        for (std::size_t i = 0; i < v.size(); ++i) {
            r = B_.CreateInsertValue(r, v[i], static_cast<unsigned>(i + 1));
        }
    }
    spillFuel();
    B_.CreateRet(r);
}

llvm::Value* FunctionCodegen::normalize(TypeId t, llvm::Value* v) {
    switch (t) {
    case ir::kI8:
        return B_.CreateSExt(B_.CreateTrunc(v, U.i8), U.i64);
    case ir::kI16:
        return B_.CreateSExt(B_.CreateTrunc(v, llvm::Type::getInt16Ty(U.C)), U.i64);
    case ir::kI32:
        return B_.CreateSExt(B_.CreateTrunc(v, U.i32), U.i64);
    case ir::kU8:
        return B_.CreateZExt(B_.CreateTrunc(v, U.i8), U.i64);
    case ir::kU16:
        return B_.CreateZExt(B_.CreateTrunc(v, llvm::Type::getInt16Ty(U.C)), U.i64);
    case ir::kU32:
        return B_.CreateZExt(B_.CreateTrunc(v, U.i32), U.i64);
    default:
        return v;
    }
}

llvm::Value* FunctionCodegen::divRem(const ir::Inst& in, llvm::Value* a, llvm::Value* b) {
    trapIf(B_.CreateICmpEQ(b, constI64(0)), ir::TrapCode::DivideByZero);
    if (!U.T.isSigned(fn_.regs[in.a])) {
        return in.op == Op::Div ? B_.CreateUDiv(a, b) : B_.CreateURem(a, b);
    }
    // parity: MIN / -1 wraps to MIN and MIN % -1 is 0, never a trap (D33)
    llvm::Value* minusOne = B_.CreateICmpEQ(b, constI64(-1));
    llvm::Value* safe = B_.CreateSelect(minusOne, constI64(1), b);
    if (in.op == Op::Rem) {
        return B_.CreateSRem(a, safe);
    }
    return B_.CreateSelect(minusOne, B_.CreateSub(constI64(0), a), B_.CreateSDiv(a, safe));
}

void FunctionCodegen::binary(const ir::Inst& in) {
    TypeId t = fn_.regs[in.a];
    llvm::Value* a = read1(in.a);
    llvm::Value* b = read1(in.b);
    llvm::Value* r = nullptr;
    if (t == ir::kF64 || t == ir::kF32) {
        switch (in.op) {
        case Op::Add:
            r = B_.CreateFAdd(a, b);
            break;
        case Op::Sub:
            r = B_.CreateFSub(a, b);
            break;
        case Op::Mul:
            r = B_.CreateFMul(a, b);
            break;
        case Op::Div:
            r = B_.CreateFDiv(a, b);
            break;
        default:
            r = B_.CreateFRem(a, b);
            break;
        }
        write1(in.dst, r);
        return;
    }
    bool sgn = U.T.isSigned(t);
    auto mask = static_cast<std::int64_t>(U.T.bitWidth(t) - 1);
    switch (in.op) {
    case Op::Add:
        r = B_.CreateAdd(a, b);
        break;
    case Op::Sub:
        r = B_.CreateSub(a, b);
        break;
    case Op::Mul:
        r = B_.CreateMul(a, b);
        break;
    case Op::Div:
    case Op::Rem:
        r = divRem(in, a, b);
        break;
    case Op::BitAnd:
        r = B_.CreateAnd(a, b);
        break;
    case Op::BitOr:
        r = B_.CreateOr(a, b);
        break;
    case Op::BitXor:
        r = B_.CreateXor(a, b);
        break;
    case Op::Shl:
        r = B_.CreateShl(a, B_.CreateAnd(b, constI64(mask)));
        break;
    case Op::Shr:
        r = sgn ? B_.CreateAShr(a, B_.CreateAnd(b, constI64(mask))) : B_.CreateLShr(a, B_.CreateAnd(b, constI64(mask)));
        break;
    default:
        error_ = "bad binary";
        return;
    }
    write1(in.dst, t == ir::kBool ? r : normalize(t, r));
}

void FunctionCodegen::unary(const ir::Inst& in) {
    TypeId t = fn_.regs[in.a];
    llvm::Value* a = read1(in.a);
    switch (in.op) {
    case Op::Neg:
        if (t == ir::kF64 || t == ir::kF32) {
            write1(in.dst, B_.CreateFNeg(a));
        } else {
            write1(in.dst, normalize(t, B_.CreateSub(constI64(0), a)));
        }
        return;
    case Op::Not:
        write1(in.dst, B_.CreateXor(a, llvm::ConstantInt::getTrue(U.C)));
        return;
    default:
        write1(in.dst, normalize(t, B_.CreateXor(a, constI64(-1))));
        return;
    }
}

void FunctionCodegen::compare(const ir::Inst& in) {
    TypeId t = fn_.regs[in.a];
    const ir::TypeInfo& info = U.T.info(t);
    llvm::Value* r = nullptr;
    if (t == ir::kF64 || t == ir::kF32) {
        llvm::Value* a = read1(in.a);
        llvm::Value* b = read1(in.b);
        switch (in.op) {
        case Op::Eq:
            r = B_.CreateFCmpOEQ(a, b);
            break;
        case Op::Ne:
            r = B_.CreateFCmpUNE(a, b);
            break;
        case Op::Lt:
            r = B_.CreateFCmpOLT(a, b);
            break;
        case Op::Le:
            r = B_.CreateFCmpOLE(a, b);
            break;
        case Op::Gt:
            r = B_.CreateFCmpOLT(b, a);
            break;
        default:
            r = B_.CreateFCmpOLE(b, a);
            break;
        }
        write1(in.dst, r);
        return;
    }
    if (t == ir::kStr || (info.kind == TypeKind::Nullable && info.elem == ir::kStr)) {
        llvm::Value* a = read1(in.a);
        llvm::Value* b = read1(in.b);
        if (in.op == Op::Eq || in.op == Op::Ne) {
            llvm::BasicBlock* from = B_.GetInsertBlock();
            llvm::BasicBlock* slow = llvm::BasicBlock::Create(U.C, "streq", F_);
            llvm::BasicBlock* done = llvm::BasicBlock::Create(U.C, "streq.end", F_);
            B_.CreateCondBr(B_.CreateICmpEQ(a, b), done, slow);
            B_.SetInsertPoint(slow);
            llvm::Value* eq = B_.CreateICmpNE(B_.CreateCall(U.runtime(Rt::StrEq), {a, b}), constI32(0));
            B_.CreateBr(done);
            B_.SetInsertPoint(done);
            llvm::PHINode* phi = B_.CreatePHI(U.i1, 2);
            phi->addIncoming(llvm::ConstantInt::getTrue(U.C), from);
            phi->addIncoming(eq, slow);
            write1(in.dst, in.op == Op::Eq ? static_cast<llvm::Value*>(phi) : B_.CreateNot(phi));
            return;
        }
        bool swap = in.op == Op::Gt || in.op == Op::Ge;
        llvm::Value* c = B_.CreateCall(U.runtime(Rt::StrCmp), {swap ? b : a, swap ? a : b});
        bool lt = in.op == Op::Lt || in.op == Op::Gt;
        write1(in.dst, lt ? B_.CreateICmpSLT(c, constI32(0)) : B_.CreateICmpSLE(c, constI32(0)));
        return;
    }
    if (info.kind == TypeKind::Nullable && !U.L.nullableIsPointer(t)) {
        Values a = read(in.a);
        Values b = read(in.b);
        llvm::Value* payload = llvm::ConstantInt::getTrue(U.C);
        for (std::size_t i = 1; i < a.size(); ++i) {
            llvm::Value* e =
                a[i]->getType()->isFloatingPointTy() ? B_.CreateFCmpOEQ(a[i], b[i]) : B_.CreateICmpEQ(a[i], b[i]);
            payload = B_.CreateAnd(payload, e);
        }
        llvm::Value* eq = B_.CreateAnd(B_.CreateICmpEQ(a[0], b[0]), B_.CreateOr(B_.CreateNot(a[0]), payload));
        write1(in.dst, in.op == Op::Eq ? eq : B_.CreateNot(eq));
        return;
    }
    llvm::Value* a = read(in.a)[0];
    llvm::Value* b = read(in.b)[0];
    if (a->getType()->isPointerTy()) {
        a = B_.CreatePtrToInt(a, U.i64);
        b = B_.CreatePtrToInt(b, U.i64);
    }
    bool uns = U.T.isInteger(t) && !U.T.isSigned(t);
    switch (in.op) {
    case Op::Eq:
        r = B_.CreateICmpEQ(a, b);
        break;
    case Op::Ne:
        r = B_.CreateICmpNE(a, b);
        break;
    case Op::Lt:
        r = uns ? B_.CreateICmpULT(a, b) : B_.CreateICmpSLT(a, b);
        break;
    case Op::Le:
        r = uns ? B_.CreateICmpULE(a, b) : B_.CreateICmpSLE(a, b);
        break;
    case Op::Gt:
        r = uns ? B_.CreateICmpULT(b, a) : B_.CreateICmpSLT(b, a);
        break;
    default:
        r = uns ? B_.CreateICmpULE(b, a) : B_.CreateICmpSLE(b, a);
        break;
    }
    write1(in.dst, r);
}

void FunctionCodegen::convert(const ir::Inst& in) {
    const ir::TypeTable& T = U.T;
    TypeId from = fn_.regs[in.a];
    TypeId to = fn_.regs[in.dst];
    llvm::Value* v = read1(in.a);
    bool fromF = T.isFloat(from);
    bool toF = T.isFloat(to);
    if (fromF && toF) {
        if (from != to) {
            v = to == ir::kF32 ? B_.CreateFPTrunc(v, U.f32) : B_.CreateFPExt(v, U.f64);
        }
        write1(in.dst, v);
        return;
    }
    if (fromF) {
        // parity: Rust `as` saturates and maps NaN to 0, which is exactly fptosi.sat / fptoui.sat
        llvm::Value* d = from == ir::kF32 ? B_.CreateFPExt(v, U.f64) : v;
        llvm::Type* n = llvm::Type::getIntNTy(U.C, static_cast<unsigned>(T.bitWidth(to)));
        bool sgn = T.isSigned(to) || !T.isInteger(to);
        llvm::Intrinsic::ID id = sgn ? llvm::Intrinsic::fptosi_sat : llvm::Intrinsic::fptoui_sat;
        llvm::Value* r = B_.CreateIntrinsic(id, {n, U.f64}, {d});
        if (n != U.i64) {
            r = sgn ? B_.CreateSExt(r, U.i64) : B_.CreateZExt(r, U.i64);
        }
        write1(in.dst, r);
        return;
    }
    if (from == ir::kBool) {
        v = B_.CreateZExt(v, U.i64);
    }
    if (toF) {
        bool uns = T.isInteger(from) && !T.isSigned(from);
        llvm::Type* ft = to == ir::kF32 ? U.f32 : U.f64;
        write1(in.dst, uns ? B_.CreateUIToFP(v, ft) : B_.CreateSIToFP(v, ft));
        return;
    }
    write1(in.dst, normalize(to, v));
}

void FunctionCodegen::call(const ir::Inst& in) {
    std::uint32_t callee = in.imm;
    bool suspends = U.IR.functions[callee].isSuspend;
    llvm::BasicBlock* retry = nullptr;
    if (live_ != nullptr) {
        // parity: a call whose fuel charge yields runs again, checks included, when the task resumes
        retry = llvm::BasicBlock::Create(U.C, "call.retry", F_);
        B_.CreateBr(retry);
        B_.SetInsertPoint(retry);
    }
    enterCall(callee, U.recursive(index_, callee), ir::resumeState(pc_, ir::ResumeAt::BeforeCall), retry);
    llvm::Value* nb = gep(base_, static_cast<std::uint64_t>(cf_.argBase) * 8);
    llvm::BasicBlock* site = nullptr;
    if (suspends) {
        site = llvm::BasicBlock::Create(U.C, "call.site", F_);
        B_.CreateBr(site);
        B_.SetInsertPoint(site);
        nb = gep(base_, static_cast<std::uint64_t>(cf_.argBase) * 8); // the resume path enters here
    }
    std::vector<llvm::Value*> args{nb, ctx_, B_.CreateAdd(depth_, constI32(1))};
    for (std::uint32_t i = 0; i < in.argCount; ++i) {
        Values v = read(fn_.args[in.argBegin + i]);
        args.insert(args.end(), v.begin(), v.end());
    }
    spillFuel();
    llvm::CallInst* r = B_.CreateCall(U.function(callee), args);
    r->setCallingConv(llvm::CallingConv::Fast);
    reloadFuel();
    if (suspends) {
        llvm::BasicBlock* failed = llvm::BasicBlock::Create(U.C, "call.failed", F_);
        llvm::BasicBlock* suspended = llvm::BasicBlock::Create(U.C, "call.suspended", F_);
        llvm::BasicBlock* ok = llvm::BasicBlock::Create(U.C, "cont", F_);
        B_.CreateCondBr(B_.CreateExtractValue(r, 0), failed, ok, U.cold);
        B_.SetInsertPoint(failed);
        llvm::Value* flag = ctxField(offsetof(vm::NativeCtx, suspending), U.i32);
        B_.CreateCondBr(B_.CreateICmpNE(flag, constI32(0)), suspended, failBlock_);
        B_.SetInsertPoint(suspended);
        std::uint32_t state = ir::resumeState(pc_, ir::ResumeAt::After);
        std::vector<ir::Reg> kept = live_->kept(pc_, ir::ResumeAt::After);
        suspendAndReturn(state, kept);
        addResume(state, std::move(kept), ir::kNoReg, site);
        B_.SetInsertPoint(ok);
    } else {
        failIf(B_.CreateExtractValue(r, 0));
    }
    if (in.dst != ir::kNoReg) {
        Values out;
        for (std::size_t i = 0; i < regs_[in.dst].size(); ++i) {
            out.push_back(B_.CreateExtractValue(r, static_cast<unsigned>(i + 1)));
        }
        write(in.dst, out);
    }
}

void FunctionCodegen::callHost(const ir::Inst& in) {
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < in.argCount; ++i) {
        total += static_cast<std::uint32_t>(kindsOf(fn_.args[in.argBegin + i]).size());
    }
    std::uint32_t resultSlots = U.P.importResultSlots[in.imm];
    llvm::Value* s = scratch(total + std::max<std::uint32_t>(resultSlots, 1));
    std::uint64_t at = 0;
    for (std::uint32_t i = 0; i < in.argCount; ++i) {
        ir::Reg r = fn_.args[in.argBegin + i];
        storeSlots(s, at, read(r), kindsOf(r), U.tbaaSlot);
        at += kindsOf(r).size();
    }
    llvm::Value* result =
        resultSlots != 0 ? gep(s, static_cast<std::uint64_t>(total) * 8) : llvm::ConstantPointerNull::get(U.ptr);
    // Host functions may charge fuel (HostCall::charge).
    llvm::Value* before = B_.CreateLoad(U.i64, fuel_);
    spillFuel();
    llvm::Value* ok = B_.CreateCall(U.runtime(Rt::CallHost), {ctx_, constI32(in.imm), s, result});
    reloadFuel();
    trapIf(B_.CreateICmpEQ(ok, constI32(0)), kAlreadySet);
    if (in.dst != ir::kNoReg && resultSlots != 0) {
        write(in.dst, loadSlots(result, 0, kindsOf(in.dst), U.tbaaSlot));
    }
    afterFuelCharge(before);
}

void FunctionCodegen::afterFuelCharge(llvm::Value* before) {
    if (recordResume_) {
        llvm::BasicBlock* resume = llvm::BasicBlock::Create(U.C, "host.resume", F_);
        B_.CreateBr(resume);
        B_.SetInsertPoint(resume);
        resume_[pc_] = resume;
    } else if (!checkFuel_) {
        // A charge voids the fuel bound of the unchecked loop copy: continue in the checked one.
        auto it = resume_.find(pc_);
        if (it != resume_.end()) {
            llvm::BasicBlock* cont = llvm::BasicBlock::Create(U.C, "cont", F_);
            llvm::Value* charged = B_.CreateICmpNE(B_.CreateLoad(U.i64, fuel_), before);
            B_.CreateCondBr(charged, it->second, cont, U.cold);
            B_.SetInsertPoint(cont);
        } else {
            error_ = "fuel charging call without a resume point";
        }
    }
}

void FunctionCodegen::field(const ir::Inst& in) {
    TypeId agg = fn_.regs[in.a];
    std::uint32_t off = U.L.fieldOffset(agg, in.imm);
    const ir::TypeInfo& info = U.T.info(agg);
    TypeId ft = info.kind == TypeKind::Tuple ? info.elems[in.imm] : info.fields[in.imm].type;
    const std::vector<SlotKind>& k = U.kinds(ft);
    if (in.op == Op::FieldGet) {
        if (info.kind == TypeKind::State) {
            write(in.dst, loadSlots(read1(in.a), off, k, U.tbaaSlot));
            return;
        }
        Values all = read(in.a);
        write(in.dst, Values(all.begin() + off, all.begin() + off + static_cast<std::ptrdiff_t>(k.size())));
        return;
    }
    Values v = read(in.b);
    if (info.kind != TypeKind::State) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            B_.CreateStore(v[i], regs_[in.a][off + i]);
        }
        return;
    }
    llvm::Value* st = read1(in.a);
    if (k.size() == 1 && !U.T.hasRefs(ft)) {
        storeSlots(st, off, v, k, U.tbaaSlot);
        return;
    }
    llvm::Value* s = scratch(static_cast<std::uint32_t>(k.size()));
    storeSlots(s, 0, v, k, U.tbaaSlot);
    llvm::Value* ok =
        B_.CreateCall(U.runtime(Rt::StateStore), {ctx_, constI32(ft), s, gep(st, static_cast<std::uint64_t>(off) * 8)});
    trapIf(B_.CreateICmpEQ(ok, constI32(0)), kAlreadySet);
}

void FunctionCodegen::nullable(const ir::Inst& in) {
    switch (in.op) {
    case Op::IsNull: {
        if (U.L.nullableIsPointer(fn_.regs[in.a])) {
            llvm::Value* v = read1(in.a);
            write1(in.dst, B_.CreateICmpEQ(v, llvm::Constant::getNullValue(v->getType())));
        } else {
            write1(in.dst, B_.CreateNot(read(in.a)[0]));
        }
        return;
    }
    case Op::Unwrap: {
        Values v = read(in.a);
        if (U.L.nullableIsPointer(fn_.regs[in.a])) {
            trapIf(B_.CreateICmpEQ(v[0], llvm::Constant::getNullValue(v[0]->getType())), ir::TrapCode::NullUnwrap);
            write(in.dst, v);
        } else {
            trapIf(B_.CreateNot(v[0]), ir::TrapCode::NullUnwrap);
            write(in.dst, Values(v.begin() + 1, v.end()));
        }
        return;
    }
    case Op::Wrap: {
        Values v = read(in.a);
        if (!U.L.nullableIsPointer(fn_.regs[in.dst])) {
            v.insert(v.begin(), llvm::ConstantInt::getTrue(U.C));
        }
        write(in.dst, v);
        return;
    }
    default: {
        Values v;
        for (SlotKind k : kindsOf(in.dst)) {
            v.push_back(zeroOf(k));
        }
        write(in.dst, v);
        return;
    }
    }
}

void FunctionCodegen::enumOp(const ir::Inst& in) {
    if (in.op == Op::EnumTag) {
        write1(in.dst, read(in.a)[0]);
        return;
    }
    if (in.op == Op::EnumMake) {
        Values v{constI64(in.imm)};
        for (std::uint32_t i = 0; i < in.argCount; ++i) {
            ir::Reg r = fn_.args[in.argBegin + i];
            Values x = read(r);
            const std::vector<SlotKind>& k = kindsOf(r);
            for (std::size_t j = 0; j < x.size(); ++j) {
                v.push_back(toBits(x[j], k[j]));
            }
        }
        while (v.size() < regs_[in.dst].size()) {
            v.push_back(constI64(0));
        }
        write(in.dst, v);
        return;
    }
    const ir::VariantInfo& var = U.T.info(fn_.regs[in.a]).variants[in.imm];
    std::size_t off = 1;
    for (std::uint32_t i = 0; i < in.b; ++i) {
        off += U.L.slots(var.payload[i]);
    }
    Values all = read(in.a);
    const std::vector<SlotKind>& k = kindsOf(in.dst);
    Values out;
    for (std::size_t j = 0; j < k.size(); ++j) {
        out.push_back(fromBits(all[off + j], k[j]));
    }
    write(in.dst, out);
}

} // namespace quartz::compiler
