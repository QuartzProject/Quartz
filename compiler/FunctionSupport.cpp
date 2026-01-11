#include "Codegen.hpp"
#include "vm/NativeAbi.hpp"

#include <cstddef>

namespace quartz::compiler {

FunctionCodegen::FunctionCodegen(UnitCodegen& unit, std::uint32_t index)
    : U(unit), fn_(unit.IR.functions[index]), cf_(unit.P.functions[index]), index_(index), F_(unit.function(index)),
      B_(unit.C), entryB_(unit.C) {}

const std::vector<SlotKind>& FunctionCodegen::kindsOf(ir::Reg r) { return U.kinds(fn_.regs[r]); }

FunctionCodegen::Values FunctionCodegen::read(ir::Reg r) {
    Values out;
    const std::vector<llvm::AllocaInst*>& slots = regs_[r];
    out.reserve(slots.size());
    for (llvm::AllocaInst* a : slots) {
        out.push_back(B_.CreateLoad(a->getAllocatedType(), a));
    }
    return out;
}

void FunctionCodegen::write(ir::Reg r, const Values& v) {
    const std::vector<llvm::AllocaInst*>& slots = regs_[r];
    for (std::size_t i = 0; i < slots.size() && i < v.size(); ++i) {
        B_.CreateStore(v[i], slots[i]);
    }
}

llvm::LoadInst* FunctionCodegen::tag(llvm::LoadInst* l, llvm::MDNode* tbaa) {
    l->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa);
    return l;
}

llvm::StoreInst* FunctionCodegen::tag(llvm::StoreInst* s, llvm::MDNode* tbaa) {
    s->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa);
    return s;
}

llvm::Value* FunctionCodegen::gep(llvm::Value* base, std::uint64_t byteOffset) {
    if (byteOffset == 0) {
        return base;
    }
    return B_.CreateConstInBoundsGEP1_64(U.i8, base, byteOffset);
}

llvm::Value* FunctionCodegen::loadField(llvm::Type* t, llvm::Value* base, std::uint64_t byteOffset,
                                        llvm::MDNode* tbaa) {
    return tag(B_.CreateLoad(t, gep(base, byteOffset)), tbaa);
}

llvm::Value* FunctionCodegen::loadSlot(llvm::Value* base, std::uint64_t index, SlotKind k, llvm::MDNode* tbaa) {
    llvm::Value* at = gep(base, index * 8);
    if (k == SlotKind::I1) {
        llvm::Value* raw = tag(B_.CreateLoad(U.i64, at), tbaa);
        return B_.CreateICmpNE(raw, constI64(0));
    }
    return tag(B_.CreateLoad(U.kindType(k), at), tbaa);
}

void FunctionCodegen::storeSlot(llvm::Value* base, std::uint64_t index, llvm::Value* v, SlotKind k,
                                llvm::MDNode* tbaa) {
    llvm::Value* at = gep(base, index * 8);
    if (k == SlotKind::I1 || k == SlotKind::F32) {
        tag(B_.CreateStore(toBits(v, k), at), tbaa);
        return;
    }
    tag(B_.CreateStore(v, at), tbaa);
}

FunctionCodegen::Values FunctionCodegen::loadSlots(llvm::Value* base, std::uint64_t first,
                                                   const std::vector<SlotKind>& k, llvm::MDNode* tbaa) {
    Values out;
    out.reserve(k.size());
    for (std::size_t i = 0; i < k.size(); ++i) {
        out.push_back(loadSlot(base, first + i, k[i], tbaa));
    }
    return out;
}

void FunctionCodegen::storeSlots(llvm::Value* base, std::uint64_t first, const Values& v,
                                 const std::vector<SlotKind>& k, llvm::MDNode* tbaa) {
    for (std::size_t i = 0; i < k.size() && i < v.size(); ++i) {
        storeSlot(base, first + i, v[i], k[i], tbaa);
    }
}

llvm::Value* FunctionCodegen::toBits(llvm::Value* v, SlotKind k) {
    switch (k) {
    case SlotKind::I1:
        return B_.CreateZExt(v, U.i64);
    case SlotKind::I64:
        return v;
    case SlotKind::F64:
        return B_.CreateBitCast(v, U.i64);
    case SlotKind::F32:
        return B_.CreateZExt(B_.CreateBitCast(v, U.i32), U.i64);
    case SlotKind::Ptr:
        return B_.CreatePtrToInt(v, U.i64);
    }
    return v;
}

llvm::Value* FunctionCodegen::fromBits(llvm::Value* bits, SlotKind k) {
    switch (k) {
    case SlotKind::I1:
        return B_.CreateICmpNE(bits, constI64(0));
    case SlotKind::I64:
        return bits;
    case SlotKind::F64:
        return B_.CreateBitCast(bits, U.f64);
    case SlotKind::F32:
        return B_.CreateBitCast(B_.CreateTrunc(bits, U.i32), U.f32);
    case SlotKind::Ptr:
        return B_.CreateIntToPtr(bits, U.ptr);
    }
    return bits;
}

llvm::Value* FunctionCodegen::zeroOf(SlotKind k) { return llvm::Constant::getNullValue(U.kindType(k)); }

llvm::Value* FunctionCodegen::scratch(std::uint32_t slots) {
    if (slots > scratchSlots_) {
        scratchSlots_ = slots;
    }
    return scratch_;
}

llvm::Value* FunctionCodegen::ctxField(std::uint64_t offset, llvm::Type* t) {
    return loadField(t, ctx_, offset, U.tbaaCtx);
}

llvm::Value* FunctionCodegen::listLen(llvm::Value* list) {
    return B_.CreateZExt(loadField(U.i32, list, offsetof(vm::List, len), U.tbaaList), U.i64);
}

void FunctionCodegen::deadBlock() { B_.SetInsertPoint(llvm::BasicBlock::Create(U.C, "dead", F_)); }

void FunctionCodegen::emitTrap(ir::TrapCode code) {
    B_.CreateCall(U.runtime(Rt::Trap),
                  {ctx_, constI32(static_cast<std::uint32_t>(code)), constI32(index_), constI32(line_)});
    B_.CreateBr(failBlock_);
}

void FunctionCodegen::trapIf(llvm::Value* failed, ir::TrapCode code) {
    llvm::BasicBlock* bad = llvm::BasicBlock::Create(U.C, "trap", F_);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(U.C, "cont", F_);
    B_.CreateCondBr(failed, bad, ok, U.cold);
    B_.SetInsertPoint(bad);
    emitTrap(code);
    B_.SetInsertPoint(ok);
}

void FunctionCodegen::failIf(llvm::Value* failed) {
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(U.C, "cont", F_);
    B_.CreateCondBr(failed, failBlock_, ok, U.cold);
    B_.SetInsertPoint(ok);
}

void FunctionCodegen::chargeFuel() {
    llvm::Value* fuel = B_.CreateSub(B_.CreateLoad(U.i64, fuel_), constI64(1));
    B_.CreateStore(fuel, fuel_);
    trapIf(B_.CreateICmpSLT(fuel, constI64(0)), ir::TrapCode::OutOfFuel);
}

void FunctionCodegen::spillFuel() {
    tag(B_.CreateStore(B_.CreateLoad(U.i64, fuel_), gep(ctx_, offsetof(vm::NativeCtx, fuel))), U.tbaaFuel);
}

void FunctionCodegen::reloadFuel() {
    B_.CreateStore(tag(B_.CreateLoad(U.i64, gep(ctx_, offsetof(vm::NativeCtx, fuel))), U.tbaaFuel), fuel_);
}

llvm::Value* FunctionCodegen::boundsFail(llvm::Value* index, llvm::Value* len) { return B_.CreateICmpUGE(index, len); }

std::string FunctionCodegen::run() {
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(U.C, "entry", F_);
    llvm::BasicBlock* body = llvm::BasicBlock::Create(U.C, "body", F_);
    entryB_.SetInsertPoint(entry);
    base_ = F_->getArg(0);
    ctx_ = F_->getArg(1);
    depth_ = F_->getArg(2);
    scratch_ = entryB_.CreateAlloca(U.i64, constI32(1), "scratch");
    // The fuel counter lives in a local so loops keep it in a register; ctx->fuel is synced around calls.
    fuel_ = entryB_.CreateAlloca(U.i64, nullptr, "fuel");
    entryB_.CreateStore(
        tag(entryB_.CreateLoad(U.i64, entryB_.CreateConstInBoundsGEP1_64(U.i8, ctx_, offsetof(vm::NativeCtx, fuel))),
            U.tbaaFuel),
        fuel_);
    regs_.resize(fn_.regs.size());
    for (std::size_t r = 0; r < fn_.regs.size(); ++r) {
        for (SlotKind k : U.kinds(fn_.regs[r])) {
            regs_[r].push_back(entryB_.CreateAlloca(U.kindType(k)));
        }
        if (regs_[r].size() != U.L.slots(fn_.regs[r])) {
            return "slot layout mismatch";
        }
    }
    unsigned arg = 3;
    for (std::uint32_t p = 0; p < fn_.paramCount; ++p) {
        for (llvm::AllocaInst* a : regs_[p]) {
            entryB_.CreateStore(F_->getArg(arg++), a);
        }
    }
    failBlock_ = llvm::BasicBlock::Create(U.C, "fail", F_);
    B_.SetInsertPoint(failBlock_);
    llvm::StructType* rt = U.resultType(index_);
    llvm::Value* failed = llvm::UndefValue::get(rt);
    failed = B_.CreateInsertValue(failed, llvm::ConstantInt::getTrue(U.C), 0);
    spillFuel();
    B_.CreateRet(failed);

    B_.SetInsertPoint(body);
    for (const ir::Inst& in : fn_.code) {
        line_ = in.line;
        lower(in);
        if (!error_.empty()) {
            return error_;
        }
    }
    if (!B_.GetInsertBlock()->hasTerminator()) {
        B_.CreateUnreachable();
    }
    if (scratchSlots_ > 1) {
        llvm::AllocaInst* big = entryB_.CreateAlloca(U.i64, constI32(scratchSlots_), "scratch");
        scratch_->replaceAllUsesWith(big);
        scratch_->eraseFromParent();
        scratch_ = big;
    }
    entryB_.CreateBr(body);
    return {};
}

} // namespace quartz::compiler
