#include "ir/Builtins.hpp"

#include "Codegen.hpp"
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
