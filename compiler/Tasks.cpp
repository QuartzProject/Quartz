#include "Codegen.hpp"
#include "vm/NativeAbi.hpp"

#include <algorithm>
#include <cstddef>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

namespace quartz::compiler {

// A suspend fn is a state machine (D56): a suspension stores the registers still live into a record on the task's
// save stack and returns as failed with ctx->suspending set; on resume the entry pops the record and jumps back.

void FunctionCodegen::zeroRefRegisters() {
    for (std::size_t r = fn_.paramCount; r < regs_.size(); ++r) {
        if (!U.T.hasRefs(fn_.regs[r])) {
            continue;
        }
        for (llvm::AllocaInst* a : regs_[r]) {
            entryB_.CreateStore(llvm::Constant::getNullValue(a->getAllocatedType()), a);
        }
    }
}

void FunctionCodegen::suspendAndReturn(std::uint32_t state, const std::vector<ir::Reg>& regs) {
    std::uint32_t total = 0;
    for (ir::Reg r : regs) {
        total += static_cast<std::uint32_t>(kindsOf(r).size());
    }
    llvm::Value* rec = scratch(std::max<std::uint32_t>(total, 1));
    std::vector<std::uint32_t> reloc;
    std::uint32_t at = 0;
    for (ir::Reg r : regs) {
        const std::vector<SlotKind>& k = kindsOf(r);
        storeSlots(rec, at, read(r), k, U.tbaaSlot);
        if (U.T.hasRefs(fn_.regs[r])) {
            reloc.push_back(at);
            reloc.push_back(fn_.regs[r]);
        }
        at += static_cast<std::uint32_t>(k.size());
    }
    llvm::Value* table = llvm::ConstantPointerNull::get(U.ptr);
    if (!reloc.empty()) {
        llvm::Constant* init = llvm::ConstantDataArray::get(U.C, reloc);
        auto* g =
            new llvm::GlobalVariable(U.M, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, "relocate");
        g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        table = g;
    }
    spillFuel();
    B_.CreateCall(U.runtime(Rt::TaskSave), {ctx_, rec, constI32(total), constI32(state), table,
                                            constI32(static_cast<std::uint32_t>(reloc.size() / 2))});
    B_.CreateBr(failBlock_);
}

void FunctionCodegen::addResume(std::uint32_t state, std::vector<ir::Reg> regs, ir::Reg waitDst,
                                llvm::BasicBlock* target) {
    for (const ResumePoint& p : resumePoints_) {
        if (p.state == state) {
            return;
        }
    }
    resumePoints_.push_back({state, std::move(regs), waitDst, target});
}

void FunctionCodegen::yieldIfOutOfFuel(llvm::Value* fuel, std::uint32_t state, llvm::BasicBlock* resume) {
    llvm::BasicBlock* out = llvm::BasicBlock::Create(U.C, "yield", F_);
    llvm::BasicBlock* cont = llvm::BasicBlock::Create(U.C, "cont", F_);
    B_.CreateCondBr(B_.CreateICmpSLT(fuel, constI64(0)), out, cont, U.cold);
    B_.SetInsertPoint(out);
    B_.CreateCall(U.runtime(Rt::TaskWait),
                  {ctx_, constI32(static_cast<std::uint32_t>(ir::WaitKind::Yield)), constI64(0), constI32(ir::kVoid)});
    std::vector<ir::Reg> kept = live_->kept(state >> 2, static_cast<ir::ResumeAt>(state & 3));
    suspendAndReturn(state, kept);
    addResume(state, std::move(kept), ir::kNoReg, resume);
    B_.SetInsertPoint(cont);
}

// parity: normal code called from a task may overdraw one fuel slice before it traps (Interp.cpp, OUT_OF_FUEL)
void FunctionCodegen::overdrawOrTrap(llvm::Value* fuel) {
    llvm::BasicBlock* over = llvm::BasicBlock::Create(U.C, "overdraw", F_);
    llvm::BasicBlock* cont = llvm::BasicBlock::Create(U.C, "cont", F_);
    B_.CreateCondBr(B_.CreateICmpSLT(fuel, constI64(0)), over, cont, U.cold);
    B_.SetInsertPoint(over);
    trapIf(B_.CreateICmpSLT(fuel, ctxField(offsetof(vm::NativeCtx, fuelFloor), U.i64)), ir::TrapCode::OutOfFuel);
    B_.CreateBr(cont);
    B_.SetInsertPoint(cont);
}

void FunctionCodegen::wait(const ir::Inst& in) {
    llvm::Value* handle = toBits(read1(in.a), kindsOf(in.a)[0]);
    ir::TypeId type = in.dst != ir::kNoReg ? fn_.regs[in.dst] : ir::kVoid;
    B_.CreateCall(U.runtime(Rt::TaskWait), {ctx_, constI32(in.imm), handle, constI32(type)});
    std::uint32_t state = ir::resumeState(pc_, ir::ResumeAt::After);
    std::vector<ir::Reg> kept = live_->kept(pc_, ir::ResumeAt::After);
    suspendAndReturn(state, kept);
    llvm::BasicBlock* resumed = llvm::BasicBlock::Create(U.C, "resumed", F_);
    addResume(state, std::move(kept), in.dst, resumed);
    B_.SetInsertPoint(resumed);
}

void FunctionCodegen::emitDispatch(llvm::BasicBlock* body) {
    if (resumePoints_.empty()) {
        entryB_.CreateBr(body);
        return;
    }
    llvm::BasicBlock* dispatch = llvm::BasicBlock::Create(U.C, "resume", F_);
    llvm::Value* top =
        tag(entryB_.CreateLoad(U.i32, entryB_.CreateConstInBoundsGEP1_64(U.i8, ctx_, offsetof(vm::NativeCtx, saveTop))),
            U.tbaaCtx);
    entryB_.CreateCondBr(entryB_.CreateICmpNE(top, constI32(0)), dispatch, body);
    B_.SetInsertPoint(dispatch);
    llvm::Value* rec = B_.CreateCall(U.runtime(Rt::TaskRestore), {ctx_});
    llvm::Value* state = ctxField(offsetof(vm::NativeCtx, resumeState), U.i32);
    llvm::BasicBlock* bad = llvm::BasicBlock::Create(U.C, "resume.bad", F_);
    llvm::SwitchInst* sw = B_.CreateSwitch(state, bad, static_cast<unsigned>(resumePoints_.size()));
    for (const ResumePoint& p : resumePoints_) {
        llvm::BasicBlock* at = llvm::BasicBlock::Create(U.C, "resume.state", F_);
        sw->addCase(llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(U.i32), p.state), at);
        B_.SetInsertPoint(at);
        std::uint64_t slot = 0;
        for (ir::Reg r : p.regs) {
            const std::vector<SlotKind>& k = kindsOf(r);
            write(r, loadSlots(rec, slot, k, U.tbaaSlot));
            slot += k.size();
        }
        if (p.waitDst != ir::kNoReg) {
            write1(p.waitDst,
                   loadSlot(gep(ctx_, offsetof(vm::NativeCtx, resumeValue)), 0, kindsOf(p.waitDst)[0], U.tbaaCtx));
        }
        B_.CreateBr(p.target);
    }
    B_.SetInsertPoint(bad);
    line_ = 0;
    emitTrap(ir::TrapCode::Unreachable);
}

} // namespace quartz::compiler
