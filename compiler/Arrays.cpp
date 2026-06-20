#include "Codegen.hpp"

namespace quartz::compiler {

// A fixed-size array register is one block of 8-byte slots, so an element is a load or store at a computed offset.
void FunctionCodegen::arrayOp(const ir::Inst& in) {
    const ir::TypeInfo& info = U.T.info(fn_.regs[in.a]);
    ir::Reg indexReg = in.op == ir::Op::ArrayGet ? in.b : fn_.args[in.argBegin];
    llvm::Value* index = read1(indexReg);
    trapIf(boundsFail(index, constI64(static_cast<std::int64_t>(info.elems.size()))), ir::TrapCode::IndexOutOfBounds);
    const std::vector<SlotKind>& k = U.kinds(info.elems[0]);
    llvm::Value* first = B_.CreateMul(index, constI64(static_cast<std::int64_t>(k.size())));
    llvm::Value* block = arrays_[in.a];
    auto slotAt = [&](std::size_t j) {
        return B_.CreateInBoundsGEP(U.i64, block, B_.CreateAdd(first, constI64(static_cast<std::int64_t>(j))));
    };
    if (in.op == ir::Op::ArrayGet) {
        Values out;
        for (std::size_t j = 0; j < k.size(); ++j) {
            out.push_back(B_.CreateLoad(U.kindType(k[j]), slotAt(j)));
        }
        write(in.dst, out);
        return;
    }
    Values v = read(in.b);
    for (std::size_t j = 0; j < k.size(); ++j) {
        B_.CreateStore(v[j], slotAt(j));
    }
}

} // namespace quartz::compiler
