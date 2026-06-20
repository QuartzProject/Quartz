#include "ir/LowerImpl.hpp"

namespace quartz::ir {

Reg Lowerer::lowerWith(WithExpr* e) {
    TypeId t = T(e->type);
    Reg source = lowerExpr(e->value, t);
    Reg out = newReg(t);
    emit(Op::Move, out, source);
    for (std::size_t i = 0; i < e->fields.size(); ++i) {
        TypeId ft = types_->info(t).fields[e->fieldIndex[i]].type;
        Reg v = lowerExpr(e->fields[i].value, ft);
        emit(Op::FieldSet, kNoReg, out, v, e->fieldIndex[i]);
    }
    return out;
}

} // namespace quartz::ir
