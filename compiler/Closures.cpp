#include "Codegen.hpp"
#include "vm/NativeAbi.hpp"

#include <cstddef>
#include <llvm/IR/Intrinsics.h>

namespace quartz::compiler {

using ir::TypeId;

void FunctionCodegen::enterCall(std::uint32_t callee, bool recursive, std::uint32_t state, llvm::BasicBlock* retry) {
    const vm::CompiledFunction& target = U.P.functions[callee];
    llvm::Value* nb = gep(base_, static_cast<std::uint64_t>(cf_.argBase) * 8);
    llvm::Value* end = gep(nb, static_cast<std::uint64_t>(target.frameSlots) * 8);
    llvm::Value* stackEnd = ctxField(offsetof(vm::NativeCtx, stackEnd), U.ptr);
    llvm::Value* maxDepth = ctxField(offsetof(vm::NativeCtx, maxDepth), U.i32);
    llvm::Value* over = B_.CreateOr(B_.CreateICmpUGT(B_.CreatePtrToInt(end, U.i64), B_.CreatePtrToInt(stackEnd, U.i64)),
                                    B_.CreateICmpUGE(depth_, maxDepth));
    trapIf(over, ir::TrapCode::StackOverflow);
    if (recursive) {
        llvm::Value* fa = B_.CreateIntrinsic(llvm::Intrinsic::frameaddress, {U.ptr}, {constI32(0)});
        llvm::Value* limit = ctxField(offsetof(vm::NativeCtx, nativeStackLimit), U.i64);
        trapIf(B_.CreateICmpULT(B_.CreatePtrToInt(fa, U.i64), limit), ir::TrapCode::StackOverflow);
    }
    chargeFuel(state, retry);
}

void FunctionCodegen::makeClosure(const ir::Inst& in) {
    if (in.argCount == 0) {
        write1(in.dst, U.closureConstant(in.imm));
        return;
    }
    std::uint32_t slots = 0;
    for (std::uint32_t i = 0; i < in.argCount; ++i) {
        slots += static_cast<std::uint32_t>(kindsOf(fn_.args[in.argBegin + i]).size());
    }
    llvm::Value* s = scratch(slots);
    std::uint32_t at = 0;
    for (std::uint32_t i = 0; i < in.argCount; ++i) {
        ir::Reg r = fn_.args[in.argBegin + i];
        storeSlots(s, at, read(r), kindsOf(r), U.tbaaSlot);
        at += static_cast<std::uint32_t>(kindsOf(r).size());
    }
    llvm::Value* c =
        B_.CreateCall(U.runtime(Rt::Closure), {ctx_, constI32(in.imm), constI32(in.argCount << 16 | slots), s});
    trapIf(B_.CreateIsNull(c), static_cast<ir::TrapCode>(vm::kTrapAlreadySet));
    write1(in.dst, c);
}

// A function value can only hold a function of its own type made into a value in this unit (the validator checks
// every closure), so the call is a switch over those; LLVM turns a known closure into a direct call.
void FunctionCodegen::callValue(const ir::Inst& in) {
    llvm::BasicBlock* retry = nullptr;
    if (live_ != nullptr) {
        retry = llvm::BasicBlock::Create(U.C, "callv.retry", F_);
        B_.CreateBr(retry);
        B_.SetInsertPoint(retry);
    }
    TypeId fnType = fn_.regs[in.a];
    std::size_t arity = U.T.info(fnType).elems.size();
    llvm::Value* clo = read1(in.a);
    llvm::Value* index = loadField(U.i32, clo, offsetof(vm::Closure, fn), U.tbaaClosure);
    Values args;
    for (std::uint32_t i = 0; i < in.argCount; ++i) {
        Values v = read(fn_.args[in.argBegin + i]);
        args.insert(args.end(), v.begin(), v.end());
    }
    std::vector<std::uint32_t> targets = U.closureTargets(fnType);
    llvm::BasicBlock* bad = llvm::BasicBlock::Create(U.C, "callv.bad", F_);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(U.C, "callv.done", F_);
    llvm::SwitchInst* sw = B_.CreateSwitch(index, bad, static_cast<unsigned>(targets.size()));
    std::uint32_t state = ir::resumeState(pc_, ir::ResumeAt::BeforeCall);
    for (std::uint32_t f : targets) {
        llvm::BasicBlock* at = llvm::BasicBlock::Create(U.C, "callv.case", F_);
        sw->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(U.i32), f), at);
        B_.SetInsertPoint(at);
        const ir::Function& target = U.IR.functions[f];
        enterCall(f, true, state, retry);
        std::vector<llvm::Value*> callArgs{gep(base_, static_cast<std::uint64_t>(cf_.argBase) * 8), ctx_,
                                           B_.CreateAdd(depth_, constI32(1))};
        callArgs.insert(callArgs.end(), args.begin(), args.end());
        std::uint64_t slot = offsetof(vm::Closure, caps) / 8;
        for (std::size_t p = arity; p < target.paramCount; ++p) {
            const std::vector<SlotKind>& k = U.kinds(target.regs[p]);
            Values cap = loadSlots(clo, slot, k, U.tbaaClosure);
            callArgs.insert(callArgs.end(), cap.begin(), cap.end());
            slot += k.size();
        }
        spillFuel();
        llvm::CallInst* r = B_.CreateCall(U.function(f), callArgs);
        r->setCallingConv(llvm::CallingConv::Fast);
        reloadFuel();
        failIf(B_.CreateExtractValue(r, 0));
        if (in.dst != ir::kNoReg) {
            Values out;
            for (std::size_t i = 0; i < kindsOf(in.dst).size(); ++i) {
                out.push_back(B_.CreateExtractValue(r, static_cast<unsigned>(i + 1)));
            }
            write(in.dst, out);
        }
        B_.CreateBr(done);
    }
    B_.SetInsertPoint(bad);
    emitTrap(ir::TrapCode::Unreachable);
    B_.SetInsertPoint(done);
}

} // namespace quartz::compiler
