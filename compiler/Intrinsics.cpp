#include "Codegen.hpp"
#include "vm/NativeAbi.hpp"

#include <cstddef>
#include <llvm/IR/Intrinsics.h>

namespace quartz::compiler {

using ir::Intrinsic;
using ir::TypeId;
using ir::TypeKind;

namespace {

constexpr auto kAlreadySet = static_cast<ir::TrapCode>(vm::kTrapAlreadySet);

} // namespace

llvm::Value* FunctionCodegen::elementAt(llvm::Value* list, llvm::Value* index, std::size_t slots, bool packed) {
    llvm::Value* data = loadField(U.ptr, list, offsetof(vm::List, data), U.tbaaList);
    if (packed) {
        return B_.CreateInBoundsGEP(U.i8, data, index);
    }
    return B_.CreateInBoundsGEP(U.i64, data, B_.CreateMul(index, constI64(static_cast<std::int64_t>(slots))));
}

FunctionCodegen::Values FunctionCodegen::loadElement(llvm::Value* at, const std::vector<SlotKind>& k, bool packed) {
    if (packed) {
        return {B_.CreateICmpNE(tag(B_.CreateLoad(U.i8, at), U.tbaaBytes), llvm::ConstantInt::get(U.i8, 0))};
    }
    return loadSlots(at, 0, k, U.tbaaSlot);
}

void FunctionCodegen::storeElement(llvm::Value* at, const Values& v, const std::vector<SlotKind>& k, bool packed) {
    if (packed) {
        tag(B_.CreateStore(B_.CreateZExt(v[0], U.i8), at), U.tbaaBytes);
        return;
    }
    storeSlots(at, 0, v, k, U.tbaaSlot);
}

void FunctionCodegen::listGet(const ir::Inst& in) {
    ir::Reg listReg = fn_.args[in.argBegin];
    llvm::Value* list = read1(listReg);
    llvm::Value* index = read1(fn_.args[in.argBegin + 1]);
    trapIf(boundsFail(index, listLen(list)), ir::TrapCode::IndexOutOfBounds);
    const std::vector<SlotKind>& k = kindsOf(in.dst);
    // parity: `[bool]` is packed one byte per element (D38)
    bool packed = fn_.regs[in.dst] == ir::kBool;
    write(in.dst, loadElement(elementAt(list, index, k.size(), packed), k, packed));
}

void FunctionCodegen::listSet(const ir::Inst& in) {
    ir::Reg listReg = fn_.args[in.argBegin];
    ir::Reg valueReg = fn_.args[in.argBegin + 2];
    TypeId elem = U.T.info(fn_.regs[listReg]).elem;
    llvm::Value* list = read1(listReg);
    llvm::Value* index = read1(fn_.args[in.argBegin + 1]);
    Values v = read(valueReg);
    const std::vector<SlotKind>& k = kindsOf(valueReg);
    bool refs = U.T.hasRefs(elem);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(U.C, "set.end", F_);
    if (refs) {
        // parity: a state-owned list of references deep copies through the runtime (Sandbox::listSet)
        llvm::BasicBlock* slow = llvm::BasicBlock::Create(U.C, "set.state", F_);
        llvm::BasicBlock* fast = llvm::BasicBlock::Create(U.C, "set.arena", F_);
        B_.CreateCondBr(needsNoCopy(list), fast, slow);
        B_.SetInsertPoint(slow);
        llvm::Value* s = scratch(static_cast<std::uint32_t>(k.size()));
        storeSlots(s, 0, v, k, U.tbaaSlot);
        llvm::Value* ok = B_.CreateCall(U.runtime(Rt::ListSet), {ctx_, list, index, s});
        trapIf(B_.CreateICmpEQ(ok, constI32(0)), kAlreadySet);
        B_.CreateBr(done);
        B_.SetInsertPoint(fast);
    }
    llvm::Value* len = listLen(list);
    if (k.size() == 1 && !refs) {
        trapIf(boundsFail(index, len), ir::TrapCode::IndexOutOfBounds);
    } else {
        // parity: these elements go through Sandbox::listSet (ListSetN), whose trap carries a message
        llvm::BasicBlock* bad = llvm::BasicBlock::Create(U.C, "trap", F_);
        llvm::BasicBlock* ok = llvm::BasicBlock::Create(U.C, "cont", F_);
        B_.CreateCondBr(boundsFail(index, len), bad, ok, U.cold);
        B_.SetInsertPoint(bad);
        B_.CreateCall(U.runtime(Rt::IndexTrap), {ctx_, index, len, constI32(index_), constI32(line_)});
        B_.CreateBr(failBlock_);
        B_.SetInsertPoint(ok);
    }
    bool packed = elem == ir::kBool;
    storeElement(elementAt(list, index, k.size(), packed), v, k, packed);
    B_.CreateBr(done);
    B_.SetInsertPoint(done);
}

// parity: a list in the state deep copies, and one in an outer `scope` region promotes (Sandbox::listSet, D36)
llvm::Value* FunctionCodegen::needsNoCopy(llvm::Value* list) {
    llvm::Value* owner = loadField(U.i8, list, offsetof(vm::List, owner), U.tbaaList);
    llvm::Value* level =
        B_.CreateZExt(loadField(llvm::Type::getInt16Ty(U.C), list, offsetof(vm::List, level), U.tbaaList), U.i32);
    llvm::Value* arena =
        B_.CreateICmpEQ(owner, llvm::ConstantInt::get(U.i8, static_cast<std::uint8_t>(vm::Owner::Arena)));
    return B_.CreateAnd(arena, B_.CreateICmpUGE(level, ctxField(offsetof(vm::NativeCtx, scopeLevel), U.i32)));
}

void FunctionCodegen::listPush(const ir::Inst& in) {
    ir::Reg listReg = fn_.args[in.argBegin];
    ir::Reg valueReg = fn_.args[in.argBegin + 1];
    TypeId elem = U.T.info(fn_.regs[listReg]).elem;
    llvm::Value* list = read1(listReg);
    Values v = read(valueReg);
    const std::vector<SlotKind>& k = kindsOf(valueReg);
    llvm::Value* len = loadField(U.i32, list, offsetof(vm::List, len), U.tbaaList);
    llvm::Value* cap = loadField(U.i32, list, offsetof(vm::List, cap), U.tbaaList);
    llvm::Value* fastOk = B_.CreateICmpULT(len, cap);
    if (U.T.hasRefs(elem)) {
        fastOk = B_.CreateAnd(fastOk, needsNoCopy(list));
    }
    llvm::BasicBlock* fast = llvm::BasicBlock::Create(U.C, "push.fast", F_);
    llvm::BasicBlock* slow = llvm::BasicBlock::Create(U.C, "push.slow", F_);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(U.C, "push.end", F_);
    B_.CreateCondBr(fastOk, fast, slow);
    B_.SetInsertPoint(fast);
    bool packed = elem == ir::kBool;
    storeElement(elementAt(list, B_.CreateZExt(len, U.i64), k.size(), packed), v, k, packed);
    tag(B_.CreateStore(B_.CreateAdd(len, constI32(1)), gep(list, offsetof(vm::List, len))), U.tbaaList);
    B_.CreateBr(done);
    B_.SetInsertPoint(slow);
    llvm::Value* s = scratch(static_cast<std::uint32_t>(k.size()));
    storeSlots(s, 0, v, k, U.tbaaSlot);
    llvm::Value* ok = B_.CreateCall(U.runtime(Rt::ListPush), {ctx_, list, s});
    trapIf(B_.CreateICmpEQ(ok, constI32(0)), kAlreadySet);
    B_.CreateBr(done);
    B_.SetInsertPoint(done);
}

void FunctionCodegen::mapGet(const ir::Inst& in) {
    ir::Reg mapReg = fn_.args[in.argBegin];
    ir::Reg keyReg = fn_.args[in.argBegin + 1];
    auto ks = static_cast<std::uint32_t>(kindsOf(keyReg).size());
    llvm::Value* s = scratch(ks);
    storeSlots(s, 0, read(keyReg), kindsOf(keyReg), U.tbaaSlot);
    llvm::Value* e = B_.CreateCall(U.runtime(Rt::MapFind), {ctx_, read1(mapReg), s});
    const std::vector<SlotKind>& k = kindsOf(in.dst);
    bool pointer = U.L.nullableIsPointer(fn_.regs[in.dst]);
    llvm::BasicBlock* from = B_.GetInsertBlock();
    llvm::BasicBlock* found = llvm::BasicBlock::Create(U.C, "map.found", F_);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(U.C, "map.end", F_);
    B_.CreateCondBr(B_.CreateIsNull(e), done, found);
    B_.SetInsertPoint(found);
    Values hit;
    if (pointer) {
        hit.push_back(loadSlot(e, ks, k[0], U.tbaaSlot));
    } else {
        hit.push_back(llvm::ConstantInt::getTrue(U.C));
        for (std::size_t i = 1; i < k.size(); ++i) {
            hit.push_back(loadSlot(e, ks + i - 1, k[i], U.tbaaSlot));
        }
    }
    B_.CreateBr(done);
    B_.SetInsertPoint(done);
    Values out;
    for (std::size_t i = 0; i < k.size(); ++i) {
        llvm::PHINode* phi = B_.CreatePHI(U.kindType(k[i]), 2);
        phi->addIncoming(zeroOf(k[i]), from);
        phi->addIncoming(hit[i], found);
        out.push_back(phi);
    }
    write(in.dst, out);
}

void FunctionCodegen::stringOp(const ir::Inst& in, Intrinsic op) {
    auto arg = [&](std::uint32_t i) { return read1(fn_.args[in.argBegin + i]); };
    auto checked = [&](llvm::Value* p) {
        trapIf(B_.CreateIsNull(p), kAlreadySet);
        write1(in.dst, p);
    };
    auto charged = [&](Rt rt, llvm::ArrayRef<llvm::Value*> args) {
        llvm::Value* p = nullptr;
        llvm::Value* before = chargingCall(rt, args, p);
        checked(p);
        afterFuelCharge(before);
    };
    auto bytes = [&](llvm::Value* s) {
        return B_.CreateZExt(loadField(U.i32, s, offsetof(vm::Str, len), U.tbaaStr), U.i64);
    };
    switch (op) {
    case Intrinsic::StrLen:
        write1(in.dst, B_.CreateZExt(loadField(U.i32, arg(0), offsetof(vm::Str, len), U.tbaaStr), U.i64));
        return;
    case Intrinsic::StrConcat: {
        llvm::Value* s = scratch(in.argCount);
        for (std::uint32_t i = 0; i < in.argCount; ++i) {
            storeSlot(s, i, arg(i), SlotKind::Ptr, U.tbaaSlot);
        }
        charged(Rt::Concat, {ctx_, s, constI32(in.argCount)});
        return;
    }
    case Intrinsic::StrSlice:
        charged(Rt::Slice, {ctx_, arg(0), arg(1), arg(2)});
        return;
    case Intrinsic::StrLower:
    case Intrinsic::StrUpper:
        charged(Rt::CaseMap, {ctx_, arg(0), constI32(op == Intrinsic::StrUpper ? 1 : 0)});
        return;
    case Intrinsic::StrContains:
    case Intrinsic::StrStartsWith:
    case Intrinsic::StrEndsWith: {
        chargeBytes(op == Intrinsic::StrContains ? B_.CreateAdd(bytes(arg(0)), bytes(arg(1))) : bytes(arg(1)));
        Rt rt = op == Intrinsic::StrContains     ? Rt::StrContains
                : op == Intrinsic::StrStartsWith ? Rt::StrStarts
                                                 : Rt::StrEnds;
        write1(in.dst, B_.CreateICmpNE(B_.CreateCall(U.runtime(rt), {arg(0), arg(1)}), constI32(0)));
        return;
    }
    case Intrinsic::StrFind: {
        chargeBytes(B_.CreateAdd(bytes(arg(0)), bytes(arg(1))));
        llvm::Value* at = B_.CreateCall(U.runtime(Rt::StrFind), {arg(0), arg(1)});
        llvm::Value* found = B_.CreateICmpSGE(at, constI64(0));
        write(in.dst, Values{found, B_.CreateSelect(found, at, constI64(0))});
        return;
    }
    case Intrinsic::StrByteAt: {
        llvm::Value* s = arg(0);
        llvm::Value* index = arg(1);
        llvm::Value* len = B_.CreateZExt(loadField(U.i32, s, offsetof(vm::Str, len), U.tbaaStr), U.i64);
        trapIf(boundsFail(index, len), ir::TrapCode::IndexOutOfBounds);
        llvm::Value* at = B_.CreateInBoundsGEP(U.i8, gep(s, offsetof(vm::Str, data)), index);
        write1(in.dst, B_.CreateZExt(tag(B_.CreateLoad(U.i8, at), U.tbaaBytes), U.i64));
        return;
    }
    case Intrinsic::StrFromI64:
        checked(B_.CreateCall(U.runtime(Rt::FromI64), {ctx_, arg(0)}));
        return;
    case Intrinsic::StrFromU64:
        checked(B_.CreateCall(U.runtime(Rt::FromU64), {ctx_, arg(0)}));
        return;
    case Intrinsic::StrFromF64:
        checked(B_.CreateCall(U.runtime(Rt::FromF64), {ctx_, arg(0)}));
        return;
    case Intrinsic::StrFromBool:
        checked(B_.CreateCall(U.runtime(Rt::FromBool), {ctx_, B_.CreateZExt(arg(0), U.i32)}));
        return;
    case Intrinsic::StrFromChar:
        checked(B_.CreateCall(U.runtime(Rt::FromChar), {ctx_, arg(0)}));
        return;
    default:
        error_ = "unsupported string intrinsic";
        return;
    }
}

llvm::Value* FunctionCodegen::javaMinMax(llvm::Value* a, llvm::Value* b, bool isMin) {
    // parity: Math.min/max in Java: canonical NaN if either is NaN, -0.0 below 0.0
    llvm::Value* nan = llvm::ConstantFP::getQNaN(U.f64);
    llvm::Value* zero = llvm::ConstantFP::get(U.f64, 0.0);
    llvm::Value* bothZero = B_.CreateAnd(B_.CreateFCmpOEQ(a, zero), B_.CreateFCmpOEQ(b, zero));
    llvm::Value* aNeg = B_.CreateICmpSLT(B_.CreateBitCast(a, U.i64), constI64(0));
    llvm::Value* zeroPick = isMin ? B_.CreateSelect(aNeg, a, b) : B_.CreateSelect(aNeg, b, a);
    llvm::Value* plain =
        isMin ? B_.CreateSelect(B_.CreateFCmpOLT(a, b), a, b) : B_.CreateSelect(B_.CreateFCmpOGT(a, b), a, b);
    llvm::Value* r = B_.CreateSelect(bothZero, zeroPick, plain);
    return B_.CreateSelect(B_.CreateFCmpUNO(a, b), nan, r);
}

void FunctionCodegen::mathOp(const ir::Inst& in, Intrinsic op) {
    auto arg = [&](std::uint32_t i) { return read1(fn_.args[in.argBegin + i]); };
    llvm::Value* r = nullptr;
    switch (op) {
    case Intrinsic::MathSqrt:
        r = B_.CreateIntrinsic(llvm::Intrinsic::sqrt, {U.f64}, {arg(0)});
        break;
    case Intrinsic::MathFloor:
        r = B_.CreateIntrinsic(llvm::Intrinsic::floor, {U.f64}, {arg(0)});
        break;
    case Intrinsic::MathCeil:
        r = B_.CreateIntrinsic(llvm::Intrinsic::ceil, {U.f64}, {arg(0)});
        break;
    case Intrinsic::MathAbsF:
        r = B_.CreateIntrinsic(llvm::Intrinsic::fabs, {U.f64}, {arg(0)});
        break;
    case Intrinsic::MathAbsI: {
        llvm::Value* x = arg(0);
        r = B_.CreateSelect(B_.CreateICmpSLT(x, constI64(0)), B_.CreateSub(constI64(0), x), x);
        break;
    }
    case Intrinsic::MathMinI:
    case Intrinsic::MathMaxI: {
        llvm::Value* a = arg(0);
        llvm::Value* b = arg(1);
        r = op == Intrinsic::MathMinI ? B_.CreateSelect(B_.CreateICmpSLT(a, b), a, b)
                                      : B_.CreateSelect(B_.CreateICmpSGT(a, b), a, b);
        break;
    }
    case Intrinsic::MathMinF:
        r = javaMinMax(arg(0), arg(1), true);
        break;
    case Intrinsic::MathMaxF:
        r = javaMinMax(arg(0), arg(1), false);
        break;
    case Intrinsic::MathPow:
        r = B_.CreateCall(U.runtime(Rt::Pow), {arg(0), arg(1)});
        break;
    case Intrinsic::MathSin:
        r = B_.CreateCall(U.runtime(Rt::Sin), {arg(0)});
        break;
    case Intrinsic::MathCos:
        r = B_.CreateCall(U.runtime(Rt::Cos), {arg(0)});
        break;
    default:
        error_ = "unsupported math intrinsic";
        return;
    }
    write1(in.dst, r);
}

void FunctionCodegen::intrinsic(const ir::Inst& in) {
    auto op = static_cast<Intrinsic>(in.imm);
    auto arg = [&](std::uint32_t i) { return read1(fn_.args[in.argBegin + i]); };
    auto checked = [&](llvm::Value* p) {
        trapIf(B_.CreateIsNull(p), kAlreadySet);
        write1(in.dst, p);
    };
    ir::Reg mapKey = in.argCount > 1 ? fn_.args[in.argBegin + 1] : ir::kNoReg;
    auto ks = static_cast<std::uint32_t>(mapKey != ir::kNoReg ? kindsOf(mapKey).size() : 0);
    auto keyScratch = [&](std::uint32_t extra) {
        ir::Reg key = mapKey;
        llvm::Value* s = scratch(ks + extra);
        storeSlots(s, 0, read(key), kindsOf(key), U.tbaaSlot);
        return s;
    };
    switch (op) {
    case Intrinsic::ListNew:
        checked(B_.CreateCall(U.runtime(Rt::ListNew), {ctx_, constI32(U.T.info(fn_.regs[in.dst]).elem)}));
        return;
    case Intrinsic::ListFilled: {
        ir::Reg value = fn_.args[in.argBegin + 1];
        llvm::Value* s = scratch(static_cast<std::uint32_t>(kindsOf(value).size()));
        storeSlots(s, 0, read(value), kindsOf(value), U.tbaaSlot);
        llvm::Value* l = nullptr;
        llvm::Value* before =
            chargingCall(Rt::ListFilled, {ctx_, arg(0), s, constI32(U.T.info(fn_.regs[in.dst]).elem)}, l);
        checked(l);
        afterFuelCharge(before);
        return;
    }
    case Intrinsic::ListLen:
        write1(in.dst, listLen(arg(0)));
        return;
    case Intrinsic::ListGet:
        listGet(in);
        return;
    case Intrinsic::ListSet:
        listSet(in);
        return;
    case Intrinsic::ListPush:
        listPush(in);
        return;
    case Intrinsic::ListPop: {
        llvm::Value* s = scratch(static_cast<std::uint32_t>(kindsOf(in.dst).size()));
        B_.CreateCall(U.runtime(Rt::ListPop), {ctx_, arg(0), s});
        write(in.dst, loadSlots(s, 0, kindsOf(in.dst), U.tbaaSlot));
        return;
    }
    case Intrinsic::ListClear: {
        TypeId elem = U.T.info(fn_.regs[fn_.args[in.argBegin]]).elem;
        if (U.T.hasRefs(elem)) {
            B_.CreateCall(U.runtime(Rt::ListClear), {ctx_, arg(0)});
        } else {
            tag(B_.CreateStore(constI32(0), gep(arg(0), offsetof(vm::List, len))), U.tbaaList);
        }
        return;
    }
    case Intrinsic::ListJoin: {
        llvm::Value* s = nullptr;
        llvm::Value* before = chargingCall(Rt::Join, {ctx_, arg(0), arg(1)}, s);
        checked(s);
        afterFuelCharge(before);
        return;
    }
    case Intrinsic::MapNew:
        checked(B_.CreateCall(U.runtime(Rt::MapNew), {ctx_, constI32(fn_.regs[in.dst])}));
        return;
    case Intrinsic::MapLen:
        write1(in.dst, B_.CreateZExt(loadField(U.i32, arg(0), offsetof(vm::Map, count), U.tbaaMap), U.i64));
        return;
    case Intrinsic::MapGet:
        mapGet(in);
        return;
    case Intrinsic::MapSet: {
        ir::Reg value = fn_.args[in.argBegin + 2];
        auto n = static_cast<std::uint32_t>(kindsOf(value).size());
        llvm::Value* s = keyScratch(n);
        storeSlots(s, ks, read(value), kindsOf(value), U.tbaaSlot);
        llvm::Value* ok = B_.CreateCall(U.runtime(Rt::MapSet), {ctx_, arg(0), s, gep(s, 8 * ks)});
        trapIf(B_.CreateICmpEQ(ok, constI32(0)), kAlreadySet);
        return;
    }
    case Intrinsic::MapHas: {
        llvm::Value* s = keyScratch(0);
        write1(in.dst, B_.CreateIsNotNull(B_.CreateCall(U.runtime(Rt::MapFind), {ctx_, arg(0), s})));
        return;
    }
    case Intrinsic::MapRemove: {
        llvm::Value* s = keyScratch(0);
        write1(in.dst, B_.CreateICmpNE(B_.CreateCall(U.runtime(Rt::MapRemove), {ctx_, arg(0), s}), constI32(0)));
        return;
    }
    case Intrinsic::StrFormat: {
        ir::Reg value = fn_.args[in.argBegin];
        llvm::Value* s = scratch(1);
        storeSlots(s, 0, read(value), kindsOf(value), U.tbaaSlot);
        checked(B_.CreateCall(U.runtime(Rt::Format), {ctx_, s, constI32(vm::formatKind(fn_.regs[value])), arg(1)}));
        return;
    }
    case Intrinsic::MapNext: {
        llvm::Value* next = B_.CreateCall(U.runtime(Rt::MapNext), {ctx_, arg(0), arg(1)});
        trapIf(B_.CreateICmpEQ(next, llvm::ConstantInt::get(U.i64, -2)), kAlreadySet);
        write1(in.dst, next);
        return;
    }
    case Intrinsic::MapKeyAt:
    case Intrinsic::MapValueAt:
    case Intrinsic::StrDecode: {
        llvm::Value* s = scratch(static_cast<std::uint32_t>(kindsOf(in.dst).size()));
        llvm::Value* ok = op == Intrinsic::StrDecode
                              ? B_.CreateCall(U.runtime(Rt::StrDecode), {ctx_, arg(0), arg(1), s})
                              : B_.CreateCall(U.runtime(Rt::MapEntry),
                                              {ctx_, arg(0), arg(1), constI32(op == Intrinsic::MapValueAt), s});
        trapIf(B_.CreateICmpEQ(ok, constI32(0)), kAlreadySet);
        write(in.dst, loadSlots(s, 0, kindsOf(in.dst), U.tbaaSlot));
        return;
    }
    case Intrinsic::Panic:
        B_.CreateCall(U.runtime(Rt::Panic), {ctx_, arg(0), constI32(index_), constI32(line_)});
        B_.CreateBr(failBlock_);
        deadBlock();
        return;
    case Intrinsic::MathSqrt:
    case Intrinsic::MathFloor:
    case Intrinsic::MathCeil:
    case Intrinsic::MathAbsF:
    case Intrinsic::MathAbsI:
    case Intrinsic::MathMinI:
    case Intrinsic::MathMaxI:
    case Intrinsic::MathMinF:
    case Intrinsic::MathMaxF:
    case Intrinsic::MathPow:
    case Intrinsic::MathSin:
    case Intrinsic::MathCos:
        mathOp(in, op);
        return;
    case Intrinsic::Count:
        error_ = "bad intrinsic";
        return;
    default:
        if (op > Intrinsic::Panic) {
            extraIntrinsic(in, op);
            return;
        }
        stringOp(in, op);
        return;
    }
}

} // namespace quartz::compiler
