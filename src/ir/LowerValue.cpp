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

Reg Lowerer::arrayIndex(Expr* index) {
    Reg idx = lowerExpr(index);
    if (T(index->type) == kI64) {
        return idx;
    }
    Reg c = newReg(kI64);
    emit(Op::Convert, c, idx);
    return c;
}

// An element store changes the array value, which then goes back to wherever the array came from.
void Lowerer::storeArray(IndexExpr* target, Reg array, Reg index, Reg value) {
    Inst& in = emitArgs(Op::ArraySet, kNoReg, 0, {index});
    in.a = array;
    in.b = value;
    Expr* object = target->object;
    if (!(object->kind == ExprKind::Ident && static_cast<IdentExpr*>(object)->ref == IdentRef::Local)) {
        assignPlace(object, array);
    }
}

Reg Lowerer::lowerArrayGet(IndexExpr* e) {
    Reg obj = lowerExpr(e->object);
    Reg idx = arrayIndex(e->index);
    Reg r = newReg(T(e->type));
    emit(Op::ArrayGet, r, obj, idx);
    return r;
}

Reg Lowerer::lowerArrayLiteral(Expr* e) {
    TypeId t = T(e->type);
    const TypeInfo& info = types_->info(t);
    TypeId elem = info.elems[0];
    std::vector<Reg> parts;
    if (e->kind == ExprKind::ListLit) {
        for (Expr* x : static_cast<ListLitExpr*>(e)->elems) {
            parts.push_back(lowerExpr(x, elem));
        }
    } else {
        auto* r = static_cast<ListRepeatExpr*>(e);
        parts.assign(info.elems.size(), lowerExpr(r->value, elem));
    }
    Reg out = newReg(t);
    emitArgs(Op::Make, out, 0, parts);
    return out;
}

Reg Lowerer::lowerArrayMethod(CallExpr* e, Reg dst) {
    TypeId t = T(e->receiver->type);
    const TypeInfo info = types_->info(t);
    if (static_cast<SpecialOp>(e->builtin) == SpecialOp::ArrayLen) {
        Reg n = constInt(static_cast<std::int64_t>(info.elems.size()), kI64);
        emit(Op::Move, dst, n);
        return dst;
    }
    Reg arr = lowerExpr(e->receiver);
    emitArgs(Op::Intrinsic, dst, static_cast<std::uint32_t>(Intrinsic::ListNew), {});
    for (std::uint32_t i = 0; i < info.elems.size(); ++i) {
        Reg x = newReg(info.elems[i]);
        emit(Op::FieldGet, x, arr, kNoReg, i);
        emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::ListPush), {dst, x});
    }
    return dst;
}

// Iterates over a copy taken before the first round, so assigning to the array's elements in the body is not seen.
void Lowerer::lowerForArray(ForStmt* s, TypeId t) {
    const TypeInfo info = types_->info(t);
    std::size_t exit = depth_;
    Reg arr = newReg(t);
    emit(Op::Move, arr, lowerExpr(s->iterable));
    Reg idx = newReg(kI64);
    emit(Op::Move, idx, constInt(0, kI64));
    Reg len = constInt(static_cast<std::int64_t>(info.elems.size()), kI64);
    Reg one = constInt(1, kI64);
    open(Op::Block);
    std::size_t loop = depth_;
    open(Op::Loop);
    Reg x = newReg(info.elems[0]);
    s->local->reg = x;
    s->local->scopeDepth = static_cast<std::uint32_t>(scopes_.size());
    emit(Op::ArrayGet, x, arr, idx);
    if (s->indexLocal != nullptr) {
        s->indexLocal->reg = newReg(kI64);
        emit(Op::Move, s->indexLocal->reg, idx);
    }
    std::size_t cont = depth_;
    open(Op::Block);
    loops_.push_back({exit, cont, scopes_.size()});
    lowerBlockStmts(s->body);
    loops_.pop_back();
    close();
    emit(Op::Add, idx, idx, one);
    Reg more = newReg(kBool);
    emit(Op::Lt, more, idx, len);
    brIf(more, loop);
    close();
    close();
}

} // namespace quartz::ir
