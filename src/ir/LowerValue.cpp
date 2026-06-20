#include "ir/LowerImpl.hpp"

#include <bit>

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

Reg Lowerer::lowerConst(const sema::ConstValue& v, TypeId t) {
    using Kind = sema::ConstValue::Kind;
    t = T(t);
    switch (v.kind) {
    case Kind::Int:
        return constInt(static_cast<std::int64_t>(v.bits), t);
    case Kind::Float:
        return constFloat(std::bit_cast<double>(v.bits), t);
    case Kind::Str:
        return constStr(v.text);
    case Kind::Null: {
        Reg r = newReg(types_->kind(t) == TypeKind::Nullable ? t : types_->nullable(kI64));
        emit(Op::NullOf, r);
        return r;
    }
    case Kind::Some: {
        Reg inner = lowerConst(v.parts[0], types_->info(t).elem);
        Reg r = newReg(t);
        emit(Op::Wrap, r, inner);
        return r;
    }
    case Kind::Aggregate:
    case Kind::Variant: {
        const TypeInfo& info = types_->info(t);
        std::vector<Reg> parts;
        for (std::size_t i = 0; i < v.parts.size(); ++i) {
            TypeId pt = v.kind == Kind::Variant         ? info.variants[v.variant].payload[i]
                        : info.kind == TypeKind::Struct ? info.fields[i].type
                                                        : info.elems[i];
            parts.push_back(lowerConst(v.parts[i], pt));
        }
        Reg r = newReg(t);
        emitArgs(v.kind == Kind::Variant ? Op::EnumMake : Op::Make, r, v.variant, parts);
        return r;
    }
    }
    return newReg(t);
}

} // namespace quartz::ir
