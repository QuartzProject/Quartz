#include "ir/Builtins.hpp"

#include "Codegen.hpp"
#include "vm/Builtins.hpp"
#include "vm/NativeAbi.hpp"

#include <llvm/IR/Intrinsics.h>

namespace quartz::compiler {

using ir::Intrinsic;

// parity: the interpreter's Builtin opcode calls the same vm::Builtins::run through qz_builtin
void FunctionCodegen::runtimeBuiltin(const ir::Inst& in, Intrinsic op) {
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < in.argCount; ++i) {
        total += static_cast<std::uint32_t>(kindsOf(fn_.args[in.argBegin + i]).size());
    }
    auto outSlots = in.dst != ir::kNoReg ? static_cast<std::uint32_t>(kindsOf(in.dst).size()) : 0;
    llvm::Value* s = scratch(total + std::max<std::uint32_t>(outSlots, 1));
    std::uint64_t at = 0;
    for (std::uint32_t i = 0; i < in.argCount; ++i) {
        ir::Reg r = fn_.args[in.argBegin + i];
        storeSlots(s, at, read(r), kindsOf(r), U.tbaaSlot);
        at += kindsOf(r).size();
    }
    llvm::Value* out = gep(s, static_cast<std::uint64_t>(total) * 8);
    llvm::Value* before = B_.CreateLoad(U.i64, fuel_);
    spillFuel();
    llvm::Value* ok = B_.CreateCall(U.runtime(Rt::Builtin), {ctx_, constI32(ir::runtimeBuiltinIndex(op)), s, out});
    reloadFuel();
    trapIf(B_.CreateICmpEQ(ok, constI32(0)), static_cast<ir::TrapCode>(vm::kTrapAlreadySet));
    if (in.dst != ir::kNoReg) {
        write(in.dst, loadSlots(out, 0, kindsOf(in.dst), U.tbaaSlot));
    }
    afterFuelCharge(before);
}

// parity: the interpreter's CHARGE_BYTES and Builtins::chargeBytes (D34)
void FunctionCodegen::chargeBytes(llvm::Value* bytes) {
    llvm::Value* units = B_.CreateLShr(bytes, constI64(vm::kFuelByteShift));
    llvm::Value* before = B_.CreateLoad(U.i64, fuel_);
    llvm::Value* left = B_.CreateSub(before, units);
    B_.CreateStore(left, fuel_);
    // Only an overdrawn budget needs the runtime, which decides between trapping and a task's overdraft.
    llvm::BasicBlock* charge = llvm::BasicBlock::Create(U.C, "charge", F_);
    llvm::BasicBlock* cont = llvm::BasicBlock::Create(U.C, "charged", F_);
    B_.CreateCondBr(B_.CreateAnd(B_.CreateICmpNE(units, constI64(0)), B_.CreateICmpSLT(left, constI64(0))), charge,
                    cont, U.cold);
    B_.SetInsertPoint(charge);
    spillFuel();
    llvm::Value* ok = B_.CreateCall(U.runtime(Rt::Charge), {ctx_, constI64(0)});
    reloadFuel();
    trapIf(B_.CreateICmpEQ(ok, constI32(0)), static_cast<ir::TrapCode>(vm::kTrapAlreadySet));
    B_.CreateBr(cont);
    B_.SetInsertPoint(cont);
    afterFuelCharge(before);
}

void FunctionCodegen::chargeCompare(llvm::Value* a, llvm::Value* b, bool equality, bool nullable) {
    auto bytes = [&] {
        llvm::Value* la = B_.CreateZExt(loadField(U.i32, a, offsetof(vm::Str, len), U.tbaaStr), U.i64);
        llvm::Value* lb = B_.CreateZExt(loadField(U.i32, b, offsetof(vm::Str, len), U.tbaaStr), U.i64);
        return equality ? B_.CreateSelect(B_.CreateICmpEQ(la, lb), la, constI64(0))
                        : B_.CreateSelect(B_.CreateICmpULT(la, lb), la, lb);
    };
    if (!nullable) {
        chargeBytes(bytes());
        return;
    }
    llvm::BasicBlock* from = B_.GetInsertBlock();
    llvm::BasicBlock* both = llvm::BasicBlock::Create(U.C, "cmp.lens", F_);
    llvm::BasicBlock* join = llvm::BasicBlock::Create(U.C, "cmp.bytes", F_);
    B_.CreateCondBr(B_.CreateAnd(B_.CreateIsNotNull(a), B_.CreateIsNotNull(b)), both, join);
    B_.SetInsertPoint(both);
    llvm::Value* n = bytes();
    llvm::BasicBlock* bothEnd = B_.GetInsertBlock();
    B_.CreateBr(join);
    B_.SetInsertPoint(join);
    llvm::PHINode* phi = B_.CreatePHI(U.i64, 2);
    phi->addIncoming(constI64(0), from);
    phi->addIncoming(n, bothEnd);
    chargeBytes(phi);
}

llvm::Value* FunctionCodegen::chargingCall(Rt rt, llvm::ArrayRef<llvm::Value*> args, llvm::Value*& result) {
    llvm::Value* before = B_.CreateLoad(U.i64, fuel_);
    spillFuel();
    result = B_.CreateCall(U.runtime(rt), args);
    reloadFuel();
    return before;
}

void FunctionCodegen::extraIntrinsic(const ir::Inst& in, Intrinsic op) {
    if (ir::isRuntimeBuiltin(op)) {
        runtimeBuiltin(in, op);
        return;
    }
    auto arg = [&](std::uint32_t i) { return read1(fn_.args[in.argBegin + i]); };
    static constexpr Rt kUnary[] = {Rt::Tan, Rt::Asin, Rt::Acos, Rt::Atan, Rt::Exp, Rt::Log, Rt::Log10};
    llvm::Value* r = nullptr;
    switch (op) {
    case Intrinsic::CharCode:
        r = arg(0);
        break;
    case Intrinsic::MathTan:
    case Intrinsic::MathAsin:
    case Intrinsic::MathAcos:
    case Intrinsic::MathAtan:
    case Intrinsic::MathExp:
    case Intrinsic::MathLog:
    case Intrinsic::MathLog10:
        r = B_.CreateCall(
            U.runtime(kUnary[static_cast<std::size_t>(op) - static_cast<std::size_t>(Intrinsic::MathTan)]), {arg(0)});
        break;
    case Intrinsic::MathRound:
        r = B_.CreateIntrinsic(llvm::Intrinsic::round, {U.f64}, {arg(0)});
        break;
    case Intrinsic::MathAtan2:
        r = B_.CreateCall(U.runtime(Rt::Atan2), {arg(0), arg(1)});
        break;
    case Intrinsic::MathFmod:
        r = B_.CreateFRem(arg(0), arg(1));
        break;
    case Intrinsic::MathClampI: {
        llvm::Value* lo = arg(1);
        llvm::Value* hi = arg(2);
        llvm::Value* x = B_.CreateSelect(B_.CreateICmpSGT(arg(0), lo), arg(0), lo);
        r = B_.CreateSelect(B_.CreateICmpSLT(x, hi), x, hi);
        break;
    }
    case Intrinsic::MathClampF:
        r = javaMinMax(javaMinMax(arg(0), arg(1), false), arg(2), true);
        break;
    default:
        error_ = "unsupported intrinsic";
        return;
    }
    write1(in.dst, r);
}

} // namespace quartz::compiler
