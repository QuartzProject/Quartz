#include "ir/LowerImpl.hpp"

namespace quartz::ir {

void Lowerer::lowerAssert(CallExpr* e) {
    Reg cond = lowerExpr(e->ordered[0], kBool);
    std::size_t done = depth_;
    open(Op::Block);
    brIf(cond, done);
    Reg message = e->ordered.size() > 1 ? lowerExpr(e->ordered[1], kStr) : constStr("assertion failed");
    emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::Panic), {message});
    emit(Op::Trap, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(TrapCode::Panic));
    close();
}

// The element a list builtin takes: literals and narrower values are lowered straight to it (`[int?].insert(0, 5)`).
TypeId Lowerer::builtinElementParam(CallExpr* e, Intrinsic op, const Expr* arg) {
    bool takesElement = (op == Intrinsic::ListInsert && e->ordered.size() > 1 && arg == e->ordered[1]) ||
                        ((op == Intrinsic::ListContains || op == Intrinsic::ListIndexOf) && arg == e->ordered[0]);
    return takesElement ? types_->info(T(e->receiver->type)).elem : kError;
}

} // namespace quartz::ir
