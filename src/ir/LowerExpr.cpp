#include "ir/Builtins.hpp"
#include "ir/LowerImpl.hpp"

#include <format>

namespace quartz::ir {

Op Lowerer::binaryOp(BinaryOp op) const {
    switch (op) {
    case BinaryOp::Add: return Op::Add;
    case BinaryOp::Sub: return Op::Sub;
    case BinaryOp::Mul: return Op::Mul;
    case BinaryOp::Div: return Op::Div;
    case BinaryOp::Rem: return Op::Rem;
    case BinaryOp::BitAnd: return Op::BitAnd;
    case BinaryOp::BitOr: return Op::BitOr;
    case BinaryOp::BitXor: return Op::BitXor;
    case BinaryOp::Shl: return Op::Shl;
    case BinaryOp::Shr: return Op::Shr;
    case BinaryOp::Eq: return Op::Eq;
    case BinaryOp::Ne: return Op::Ne;
    case BinaryOp::Lt: return Op::Lt;
    case BinaryOp::Le: return Op::Le;
    case BinaryOp::Gt: return Op::Gt;
    case BinaryOp::Ge: return Op::Ge;
    default: return Op::Trap;
    }
}

Reg Lowerer::coerce(Reg r, TypeId from, TypeId to) {
    if (r == kNoReg || to == kError || from == to || from == kNever) {
        return r;
    }
    if (types_->kind(to) == TypeKind::Nullable) {
        Reg out = newReg(to);
        if (from == kNull) {
            emit(Op::NullOf, out);
        } else if (types_->kind(from) == TypeKind::Nullable) {
            return r;
        } else {
            emit(Op::Wrap, out, r);
        }
        return out;
    }
    return r;
}

Reg Lowerer::lowerExpr(Expr* e, TypeId want) {
    if (auto pre = preLowered_.find(e); pre != preLowered_.end()) {
        return pre->second;
    }
    Reg r = lowerInner(e);
    if (T(e->type) == kNever) {
        TypeId t = want != kError ? want : kI64;
        return newReg(t == kVoid ? kI64 : t);
    }
    return coerce(r, T(e->type), want);
}

Reg Lowerer::toStr(Reg r, TypeId t) {
    if (t == kStr) {
        return r;
    }
    if (!types_->isScalar(t)) {
        return showOf(r, t);
    }
    Intrinsic op = Intrinsic::StrFromI64;
    Reg src = r;
    if (types_->isInteger(t)) {
        if (t == kU64) {
            op = Intrinsic::StrFromU64;
        } else if (t != kI64) {
            src = newReg(kI64);
            emit(Op::Convert, src, r);
        }
    } else if (types_->isFloat(t)) {
        op = Intrinsic::StrFromF64;
        if (t != kF64) {
            src = newReg(kF64);
            emit(Op::Convert, src, r);
        }
    } else if (t == kBool) {
        op = Intrinsic::StrFromBool;
    } else if (t == kChar) {
        op = Intrinsic::StrFromChar;
    }
    Reg out = newReg(kStr);
    emitArgs(Op::Intrinsic, out, static_cast<std::uint32_t>(op), {src});
    return out;
}

Reg Lowerer::lowerInterp(InterpExpr* e) {
    std::vector<Reg> parts;
    for (std::size_t i = 0; i < e->texts.size(); ++i) {
        if (!e->texts[i].empty()) {
            parts.push_back(constStr(e->texts[i]));
        }
        if (i < e->exprs.size()) {
            parts.push_back(toStr(lowerExpr(e->exprs[i]), T(e->exprs[i]->type)));
        }
    }
    if (parts.size() == 1) {
        return parts[0];
    }
    Reg out = newReg(kStr);
    emitArgs(Op::Intrinsic, out, static_cast<std::uint32_t>(Intrinsic::StrConcat), parts);
    return out;
}

Reg Lowerer::lowerBinary(BinaryExpr* e) {
    TypeId t = T(e->type);
    if (e->op == BinaryOp::And || e->op == BinaryOp::Or) {
        Reg res = newReg(kBool);
        Reg l = lowerExpr(e->lhs, kBool);
        emit(Op::Move, res, l);
        open(Op::If, res);
        if (e->op == BinaryOp::And) {
            emit(Op::Move, res, lowerExpr(e->rhs, kBool));
        } else {
            emit(Op::Else);
            emit(Op::Move, res, lowerExpr(e->rhs, kBool));
        }
        close();
        return res;
    }
    if (e->op == BinaryOp::Coalesce) {
        Reg l = lowerExpr(e->lhs);
        Reg res = newReg(t);
        Reg isNull = newReg(kBool);
        emit(Op::IsNull, isNull, l);
        open(Op::If, isNull);
        Reg r = lowerExpr(e->rhs, t);
        if (T(e->rhs->type) != kNever) {
            emit(Op::Move, res, r);
        }
        emit(Op::Else);
        if (types_->kind(t) == TypeKind::Nullable) {
            emit(Op::Move, res, l);
        } else {
            emit(Op::Unwrap, res, l);
        }
        close();
        return res;
    }
    bool compare = e->op >= BinaryOp::Eq && e->op <= BinaryOp::Ge;
    TypeId operand = T(e->lhs->type);
    if (compare && T(e->lhs->type) != T(e->rhs->type)) {
        operand = types_->kind(T(e->lhs->type)) == TypeKind::Nullable || T(e->rhs->type) == kNull ? T(e->lhs->type)
                                                                                            : T(e->rhs->type);
    }
    bool shift = e->op == BinaryOp::Shl || e->op == BinaryOp::Shr;
    Reg l = lowerExpr(e->lhs, compare ? operand : kError);
    Reg r = lowerExpr(e->rhs, compare ? operand : kError);
    if (compare && T(e->lhs->type) != kNull && T(e->rhs->type) != kNull &&
        !((e->op == BinaryOp::Eq || e->op == BinaryOp::Ne) ? vmEquatable(operand) : vmOrdered(operand))) {
        return lowerTraitCompare(e->op, l, r, operand);
    }
    if (compare && (T(e->lhs->type) == kNull || T(e->rhs->type) == kNull) && !vmEquatable(operand)) {
        Reg isNull = newReg(kBool);
        emit(Op::IsNull, isNull, T(e->lhs->type) == kNull ? r : l);
        if (e->op == BinaryOp::Eq) {
            return isNull;
        }
        Reg res = newReg(kBool);
        emit(Op::Not, res, isNull);
        return res;
    }
    if (shift && T(e->rhs->type) != t) {
        Reg conv = newReg(t);
        emit(Op::Convert, conv, r);
        r = conv;
    }
    Reg res = newReg(t);
    if (e->op == BinaryOp::Add && t == kStr) {
        emitArgs(Op::Intrinsic, res, static_cast<std::uint32_t>(Intrinsic::StrConcat), {l, r});
        return res;
    }
    emit(binaryOp(e->op), res, l, r);
    return res;
}

Reg Lowerer::lowerMember(MemberExpr* e) {
    if (e->global != nullptr) {
        return lowerExpr(e->global->decl->value, e->global->type);
    }
    if (e->isVariant) {
        Reg r = newReg(T(e->type));
        emitArgs(Op::EnumMake, r, e->fieldIndex, {});
        return r;
    }
    Reg obj = lowerExpr(e->object);
    TypeId fieldType = e->safe ? types_->info(T(e->type)).elem : T(e->type);
    if (types_->kind(T(e->type)) == TypeKind::Nullable && e->safe) {
        TypeId objInner = types_->info(T(e->object->type)).elem;
        (void)objInner;
    }
    auto read = [&](Reg base) {
        Reg out = newReg(fieldType);
        if (e->isProperty) {
            emitArgs(Op::CallHost, out, e->getter, {base});
        } else {
            emit(Op::FieldGet, out, base, kNoReg, e->fieldIndex);
        }
        return out;
    };
    if (!e->safe) {
        return read(obj);
    }
    // `a?.b`: null stays null, otherwise read the member and wrap it.
    Reg res = newReg(T(e->type));
    Reg isNull = newReg(kBool);
    emit(Op::IsNull, isNull, obj);
    open(Op::If, isNull);
    emit(Op::NullOf, res);
    emit(Op::Else);
    Reg inner = newReg(types_->info(T(e->object->type)).elem);
    emit(Op::Unwrap, inner, obj);
    Reg v = read(inner);
    if (types_->kind(fieldType) == TypeKind::Nullable) {
        emit(Op::Move, res, v);
    } else {
        emit(Op::Wrap, res, v);
    }
    close();
    return res;
}

Reg Lowerer::lowerCall(CallExpr* e) {
    if (!e->safeCall) {
        return lowerCallCore(e);
    }
    // `a?.m(...)`: null stays null, otherwise call on the unwrapped receiver.
    TypeId outer = T(e->type);
    Reg res = outer == kVoid ? kNoReg : newReg(outer);
    Reg obj = lowerInner(e->receiver);
    Reg isNull = newReg(kBool);
    emit(Op::IsNull, isNull, obj);
    std::size_t endFrame = depth_;
    open(Op::Block);
    std::size_t nullFrame = depth_;
    open(Op::Block);
    brIf(isNull, nullFrame);
    Reg inner = newReg(types_->info(T(e->receiver->type)).elem);
    emit(Op::Unwrap, inner, obj);
    preLowered_[e->receiver] = inner;
    Reg v = lowerCallCore(e);
    preLowered_.erase(e->receiver);
    if (res != kNoReg) {
        if (T(e->innerType) == outer) {
            emit(Op::Move, res, v);
        } else {
            emit(Op::Wrap, res, v);
        }
    }
    br(endFrame);
    close();
    if (res != kNoReg) {
        emit(Op::NullOf, res);
    }
    close();
    return res;
}

Reg Lowerer::lowerCallCore(CallExpr* e) {
    TypeId rt = T(e->safeCall ? e->innerType : e->type);
    Reg dst = rt == kVoid || rt == kNever ? kNoReg : newReg(rt);
    std::vector<Reg> args;
    switch (e->callKind) {
    case CallKind::Function:
    case CallKind::Host: {
        sema::FnInfo* fn = e->fn;
        std::vector<TypeId> targs;
        for (std::uint32_t a : e->typeArgs) {
            targs.push_back(T(a));
        }
        if (e->traitCall) {
            // Static dispatch: the receiver's concrete type picks the impl.
            TypeId recvType = T(e->receiver->type);
            if (e->safeCall) {
                recvType = types_->info(recvType).elem;
            }
            const TypeInfo& recvInfo = types_->info(recvType);
            std::string method = fn->name.substr(fn->name.rfind('.') + 1);
            auto it = prog_.methodTable.find({recvInfo.genericBase != 0 ? recvInfo.genericBase : recvType, method});
            bool userType = recvInfo.kind == TypeKind::Struct || recvInfo.kind == TypeKind::Enum;
            if (fn->trait != nullptr && fn->trait->lang != sema::LangTrait::None &&
                (it == prog_.methodTable.end() || !userType)) {
                return langTraitCall(e, recvType, dst);
            }
            if (it == prog_.methodTable.end()) {
                diags_.error(e->span, std::format("no implementation of '{}' for '{}'", fn->name, types_->name(recvType)));
                return dst;
            }
            fn = it->second;
            targs = recvInfo.genericBase != 0 ? recvInfo.typeArgs : std::vector<TypeId>{};
            if (fn->trait != nullptr) {
                targs = {recvType}; // a trait's default body, generic over `Self`
            }
        }
        ir::TypeSubst callee;
        for (std::size_t i = 0; i < fn->typeParams.size() && i < targs.size(); ++i) {
            callee.emplace_back(fn->typeParams[i], targs[i]);
        }
        auto paramType = [&](std::size_t i) { return types_->subst(fn->params[i], callee); };
        std::size_t p = 0;
        if (e->receiver != nullptr) {
            args.push_back(lowerExpr(e->receiver, paramType(0)));
            p = 1;
        }
        for (Expr* a : e->ordered) {
            args.push_back(lowerExpr(a, paramType(p++)));
        }
        std::uint32_t target = fn->generic() ? instance(fn, targs) : fn->index;
        emitArgs(e->callKind == CallKind::Function ? Op::Call : Op::CallHost, dst, target, args);
        return dst;
    }
    case CallKind::Special: {
        if (static_cast<SpecialOp>(e->builtin) == SpecialOp::Assert) {
            lowerAssert(e);
            return dst;
        }
        Reg recv = lowerExpr(e->receiver);
        TypeId rt0 = T(e->receiver->type);
        auto op = static_cast<SpecialOp>(e->builtin);
        bool isResult = op == SpecialOp::ResultIsOk || op == SpecialOp::ResultIsErr || op == SpecialOp::ResultUnwrap ||
                        op == SpecialOp::ResultUnwrapOr;
        Reg present = newReg(kBool);
        if (isResult) {
            Reg tag = newReg(kI64);
            emit(Op::EnumTag, tag, recv);
            emit(Op::Eq, present, tag, constInt(0, kI64));
        } else {
            Reg isNull = newReg(kBool);
            emit(Op::IsNull, isNull, recv);
            emit(Op::Not, present, isNull);
        }
        if (op == SpecialOp::ResultIsOk) {
            return present;
        }
        if (op == SpecialOp::ResultIsErr) {
            emit(Op::Not, dst, present);
            return dst;
        }
        auto extract = [&](Reg out) {
            if (isResult) {
                Inst& f = emit(Op::EnumField, out, recv, kNoReg, 0);
                f.b = 0;
            } else {
                emit(Op::Unwrap, out, recv);
            }
        };
        (void)rt0;
        if (op == SpecialOp::ResultUnwrap || op == SpecialOp::NullUnwrap) {
            std::size_t done = depth_;
            open(Op::Block);
            brIf(present, done);
            emit(Op::Trap, kNoReg, kNoReg, kNoReg,
                 static_cast<std::uint32_t>(isResult ? TrapCode::Panic : TrapCode::NullUnwrap));
            close();
            extract(dst);
            return dst;
        }
        std::size_t endFrame = depth_;
        open(Op::Block);
        std::size_t elseFrame = depth_;
        open(Op::Block);
        Reg absent = newReg(kBool);
        emit(Op::Not, absent, present);
        brIf(absent, elseFrame);
        extract(dst);
        br(endFrame);
        close();
        emit(Op::Move, dst, lowerExpr(e->ordered[0], rt));
        close();
        return dst;
    }
    case CallKind::Construct: {
        const TypeInfo& info = types_->info(rt);
        for (std::size_t i = 0; i < e->ordered.size(); ++i) {
            args.push_back(lowerExpr(e->ordered[i], info.fields[i].type));
        }
        emitArgs(Op::Make, dst, 0, args);
        return dst;
    }
    case CallKind::Variant: {
        const VariantInfo& v = types_->info(rt).variants[e->variant];
        for (std::size_t i = 0; i < e->ordered.size(); ++i) {
            args.push_back(lowerExpr(e->ordered[i], v.payload[i]));
        }
        emitArgs(Op::EnumMake, dst, e->variant, args);
        return dst;
    }
    case CallKind::Builtin: {
        auto op = static_cast<Intrinsic>(e->builtin);
        if (e->receiver != nullptr) {
            args.push_back(lowerExpr(e->receiver));
        }
        // Parameter types for coercion come from the arguments' checked types, except nullable-free builtins.
        for (Expr* a : e->ordered) {
            TypeId want = kError;
            if (TypeId elem = builtinElementParam(e, op, a); elem != kError) {
                want = elem;
            } else if (op == Intrinsic::ListPush || op == Intrinsic::MapSet) {
                const TypeInfo& recv = types_->info(T(e->receiver->type));
                want = op == Intrinsic::ListPush ? recv.elem : (a == e->ordered[0] ? recv.key : recv.elem);
            } else if (op == Intrinsic::MapGet || op == Intrinsic::MapHas || op == Intrinsic::MapRemove) {
                want = types_->info(T(e->receiver->type)).key;
            } else if (types_->isNumeric(T(a->type)) && op != Intrinsic::MathAbsI && op != Intrinsic::MathAbsF &&
                       op != Intrinsic::MathMinI && op != Intrinsic::MathMaxI && op != Intrinsic::MathMinF &&
                       op != Intrinsic::MathMaxF) {
                want = T(a->type);
            }
            Reg r = lowerExpr(a, want);
            if ((op == Intrinsic::StrSlice || op == Intrinsic::StrByteAt) && T(a->type) != kI64) {
                Reg c = newReg(kI64);
                emit(Op::Convert, c, r);
                r = c;
            }
            args.push_back(r);
        }
        if (isIntArithmetic(op)) {
            args.push_back(constInt(static_cast<std::int64_t>(T(e->receiver->type)), kI64));
        }
        emitArgs(Op::Intrinsic, dst, e->builtin, args);
        if (op == Intrinsic::Panic) {
            emit(Op::Trap, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(TrapCode::Panic));
        }
        return dst;
    }
    case CallKind::Indirect:
        return lowerIndirectCall(e, dst);
    default: break;
    }
    return dst;
}

Reg Lowerer::lowerInner(Expr* e) {
    switch (e->kind) {
    case ExprKind::IntLit: {
        auto v = static_cast<std::int64_t>(static_cast<IntLitExpr*>(e)->value);
        return types_->isFloat(T(e->type)) ? constFloat(static_cast<double>(v), T(e->type)) : constInt(v, T(e->type));
    }
    case ExprKind::FloatLit: return constFloat(static_cast<FloatLitExpr*>(e)->value, T(e->type));
    case ExprKind::BoolLit: return constInt(static_cast<BoolLitExpr*>(e)->value ? 1 : 0, kBool);
    case ExprKind::CharLit: return constInt(static_cast<CharLitExpr*>(e)->value, kChar);
    case ExprKind::StringLit: return constStr(static_cast<StringLitExpr*>(e)->value);
    case ExprKind::NullLit: {
        Reg r = newReg(T(e->type) == kNull ? types_->nullable(kI64) : T(e->type));
        emit(Op::NullOf, r);
        return r;
    }
    case ExprKind::Interp: return lowerInterp(static_cast<InterpExpr*>(e));
    case ExprKind::Ident: {
        auto* id = static_cast<IdentExpr*>(e);
        if (id->ref == IdentRef::Local) {
            return id->local->reg;
        }
        if (id->ref == IdentRef::Const) {
            return lowerExpr(id->global->decl->value, T(id->global->type));
        }
        if (id->ref == IdentRef::Function) {
            return lowerFunctionValue(id);
        }
        return newReg(kError);
    }
    case ExprKind::SelfRef: return static_cast<SelfExpr*>(e)->local->reg;
    case ExprKind::Unary: {
        auto* u = static_cast<UnaryExpr*>(e);
        if (u->op == UnaryOp::Neg && u->operand->kind == ExprKind::IntLit) {
            auto v = static_cast<std::int64_t>(0ULL - static_cast<IntLitExpr*>(u->operand)->value);
            return types_->isFloat(T(e->type)) ? constFloat(static_cast<double>(v), T(e->type)) : constInt(v, T(e->type));
        }
        if (u->op == UnaryOp::Neg && u->operand->kind == ExprKind::FloatLit) {
            return constFloat(-static_cast<FloatLitExpr*>(u->operand)->value, T(e->type));
        }
        Reg v = lowerExpr(u->operand);
        Reg r = newReg(T(e->type));
        emit(u->op == UnaryOp::Neg ? Op::Neg : u->op == UnaryOp::Not ? Op::Not : Op::BitNot, r, v);
        return r;
    }
    case ExprKind::Binary: return lowerBinary(static_cast<BinaryExpr*>(e));
    case ExprKind::Call: return lowerCall(static_cast<CallExpr*>(e));
    case ExprKind::Member: return lowerMember(static_cast<MemberExpr*>(e));
    case ExprKind::Index: {
        auto* ix = static_cast<IndexExpr*>(e);
        Reg obj = lowerExpr(ix->object);
        bool isMap = types_->kind(T(ix->object->type)) == TypeKind::Map;
        Reg idx = lowerExpr(ix->index, isMap ? types_->info(T(ix->object->type)).key : kError);
        if (!isMap && T(ix->index->type) != kI64) {
            Reg c = newReg(kI64);
            emit(Op::Convert, c, idx);
            idx = c;
        }
        if (isMap) {
            Reg opt = newReg(types_->nullable(T(e->type)));
            emitArgs(Op::Intrinsic, opt, static_cast<std::uint32_t>(Intrinsic::MapGet), {obj, idx});
            Reg r = newReg(T(e->type));
            emit(Op::Unwrap, r, opt);
            return r;
        }
        Reg r = newReg(T(e->type));
        emitArgs(Op::Intrinsic, r, static_cast<std::uint32_t>(Intrinsic::ListGet), {obj, idx});
        return r;
    }
    case ExprKind::Propagate: {
        auto* p = static_cast<PropagateExpr*>(e);
        Reg v = lowerExpr(p->inner);
        if (types_->isResult(T(p->inner->type))) {
            Reg tag = newReg(kI64);
            emit(Op::EnumTag, tag, v);
            Reg errTag = constInt(1, kI64);
            Reg isErr = newReg(kBool);
            emit(Op::Eq, isErr, tag, errTag);
            std::size_t ok = depth_;
            open(Op::Block);
            Reg isOk = newReg(kBool);
            emit(Op::Not, isOk, isErr);
            brIf(isOk, ok);
            Reg err = newReg(types_->info(T(p->inner->type)).variants[1].payload[0]);
            Inst& ef = emit(Op::EnumField, err, v, kNoReg, 1);
            ef.b = 0;
            Reg out = newReg(fn_->result);
            emitArgs(Op::EnumMake, out, 1, {err});
            emit(Op::Return, kNoReg, out);
            close();
            Reg r = newReg(T(e->type));
            Inst& vf = emit(Op::EnumField, r, v, kNoReg, 0);
            vf.b = 0;
            return r;
        }
        Reg isNull = newReg(kBool);
        emit(Op::IsNull, isNull, v);
        open(Op::If, isNull);
        Reg n = newReg(fn_->result);
        emit(Op::NullOf, n);
        emit(Op::Return, kNoReg, n);
        close();
        Reg r = newReg(T(e->type));
        emit(Op::Unwrap, r, v);
        return r;
    }
    case ExprKind::ListLit: {
        auto* l = static_cast<ListLitExpr*>(e);
        TypeId elem = types_->info(T(e->type)).elem;
        Reg r = newReg(T(e->type));
        emitArgs(Op::Intrinsic, r, static_cast<std::uint32_t>(Intrinsic::ListNew), {});
        for (Expr* x : l->elems) {
            Reg v = lowerExpr(x, elem);
            emitArgs(Op::Intrinsic, kNoReg, static_cast<std::uint32_t>(Intrinsic::ListPush), {r, v});
        }
        return r;
    }
    case ExprKind::ListRepeat: {
        auto* l = static_cast<ListRepeatExpr*>(e);
        TypeId elem = types_->info(T(e->type)).elem;
        Reg n = lowerExpr(l->count);
        if (T(l->count->type) != kI64) {
            Reg c = newReg(kI64);
            emit(Op::Convert, c, n);
            n = c;
        }
        Reg v = lowerExpr(l->value, elem);
        Reg r = newReg(T(e->type));
        emitArgs(Op::Intrinsic, r, static_cast<std::uint32_t>(Intrinsic::ListFilled), {n, v});
        return r;
    }
    case ExprKind::TupleLit: {
        auto* t = static_cast<TupleLitExpr*>(e);
        std::vector<Reg> parts;
        const TypeInfo& info = types_->info(T(e->type));
        for (std::size_t i = 0; i < t->elems.size(); ++i) {
            parts.push_back(lowerExpr(t->elems[i], info.elems[i]));
        }
        Reg r = newReg(T(e->type));
        emitArgs(Op::Make, r, 0, parts);
        return r;
    }
    case ExprKind::Cast: {
        auto* c = static_cast<CastExpr*>(e);
        Reg v = lowerExpr(c->value);
        if (T(c->value->type) == T(e->type)) {
            return v;
        }
        Reg r = newReg(T(e->type));
        emit(Op::Convert, r, v);
        return r;
    }
    case ExprKind::Wait: {
        auto* w = static_cast<WaitExpr*>(e);
        Reg v = lowerExpr(w->value);
        Reg dst = T(e->type) == kVoid ? kNoReg : newReg(T(e->type));
        emit(Op::Wait, dst, v, kNoReg, static_cast<std::uint32_t>(WaitKind::Wait));
        return dst;
    }
    case ExprKind::If: return lowerIf(static_cast<IfExpr*>(e), T(e->type), true);
    case ExprKind::Match: return lowerMatch(static_cast<MatchExpr*>(e), T(e->type), true);
    case ExprKind::Block: return lowerBlockValue(static_cast<BlockExpr*>(e), T(e->type));
    case ExprKind::Lambda:
        return lowerClosure(static_cast<LambdaExpr*>(e));
    default: break;
    }
    return newReg(kError);
}

Reg Lowerer::patternTest(Pattern* p, Reg subject, TypeId subjectType) {
    switch (p->kind) {
    case PatternKind::Wildcard:
    case PatternKind::Binding: return kNoReg;
    case PatternKind::Literal: {
        Reg lit = lowerExpr(p->literal, subjectType);
        Reg c = newReg(kBool);
        emit(Op::Eq, c, subject, lit);
        return c;
    }
    case PatternKind::Range: {
        Reg lo = lowerExpr(p->literal, subjectType);
        Reg hi = lowerExpr(p->rangeHi, subjectType);
        Reg a = newReg(kBool);
        emit(Op::Ge, a, subject, lo);
        Reg b = newReg(kBool);
        emit(p->inclusive ? Op::Le : Op::Lt, b, subject, hi);
        Reg c = newReg(kBool);
        emit(Op::BitAnd, c, a, b);
        return c;
    }
    case PatternKind::Variant: {
        Reg tag = newReg(kI64);
        emit(Op::EnumTag, tag, subject);
        Reg want = constInt(p->variantIndex, kI64);
        Reg c = newReg(kBool);
        emit(Op::Eq, c, tag, want);
        return c;
    }
    }
    return kNoReg;
}

void Lowerer::bindPattern(Pattern* p, Reg subject, TypeId subjectType) {
    if (p->kind == PatternKind::Binding && p->local != nullptr) {
        p->local->reg = newReg(subjectType);
        emit(Op::Move, p->local->reg, subject);
    } else if (p->kind == PatternKind::Variant) {
        const VariantInfo& v = types_->info(subjectType).variants[p->variantIndex];
        for (std::uint32_t i = 0; i < p->sub.size(); ++i) {
            Pattern* sub = p->sub[i];
            if (sub->kind == PatternKind::Binding && sub->local != nullptr) {
                sub->local->reg = newReg(v.payload[i]);
                Inst& in = emit(Op::EnumField, sub->local->reg, subject, kNoReg, p->variantIndex);
                in.b = i;
            }
        }
    }
}

Reg Lowerer::lowerArmBody(Expr* body, TypeId want, bool wantValue) {
    if (wantValue) {
        return lowerExpr(body, want);
    }
    if (body->kind == ExprKind::Block) {
        lowerBlockStmts(static_cast<BlockExpr*>(body));
    } else if (body->kind == ExprKind::If) {
        lowerIf(static_cast<IfExpr*>(body), kError, false);
    } else if (body->kind == ExprKind::Match) {
        lowerMatch(static_cast<MatchExpr*>(body), kError, false);
    } else {
        lowerExpr(body);
    }
    return kNoReg;
}

Reg Lowerer::lowerMatch(MatchExpr* m, TypeId want, bool wantValue) {
    TypeId st = T(m->subject->type);
    Reg subject = lowerExpr(m->subject);
    TypeId rt = wantValue ? (want != kError ? want : T(m->type)) : kError;
    Reg result = wantValue && rt != kVoid && rt != kNever ? newReg(rt) : kNoReg;
    std::size_t exit = depth_;
    open(Op::Block);
    for (MatchArm& arm : m->arms) {
        std::size_t next = depth_;
        open(Op::Block);
        Reg test = patternTest(arm.pattern, subject, st);
        if (test != kNoReg) {
            Reg miss = newReg(kBool);
            emit(Op::Not, miss, test);
            brIf(miss, next);
        }
        bindPattern(arm.pattern, subject, st);
        if (arm.guard != nullptr) {
            Reg g = lowerExpr(arm.guard, kBool);
            Reg miss = newReg(kBool);
            emit(Op::Not, miss, g);
            brIf(miss, next);
        }
        Reg v = lowerArmBody(arm.body, rt, wantValue);
        if (result != kNoReg && v != kNoReg && T(arm.body->type) != kNever) {
            emit(Op::Move, result, v);
        }
        br(exit);
        close();
    }
    emit(Op::Trap, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(TrapCode::Unreachable));
    close();
    return result;
}

std::unique_ptr<Module> Lowerer::run() {
    mod_->functions.resize(prog_.functions.size());
    nextFn_ = static_cast<std::uint32_t>(prog_.functions.size());
    for (sema::FnInfo* imp : prog_.imports) {
        mod_->imports.push_back({imp->name, imp->params, imp->result});
    }
    for (sema::FnInfo* fn : prog_.functions) {
        lowerFunction(fn, mod_->functions[fn->index]);
    }
    for (sema::FnInfo* fn : prog_.functions) {
        for (sema::AnnotationInfo& a : fn->annotations) {
            Annotation out{a.name, fn->index, a.args, ~0U};
            if (a.where != nullptr) {
                out.filter = allocFunction();
                lowerFilter(a, fn, function(out.filter));
            }
            mod_->annotations.push_back(std::move(out));
        }
    }
    for (TypeId state : prog_.states) {
        mod_->states.push_back(state);
        std::uint32_t idx = allocFunction();
        mod_->stateInit.push_back(idx);
        lowerStateInit(state, function(idx));
    }
    while (!work_.empty() || !lambdaWork_.empty()) {
        if (!lambdaWork_.empty()) {
            LambdaWork w = std::move(lambdaWork_.back());
            lambdaWork_.pop_back();
            lowerLambda(w);
            continue;
        }
        auto [fn, args, idx] = work_.back();
        work_.pop_back();
        subst_.clear();
        for (std::size_t i = 0; i < fn->typeParams.size() && i < args.size(); ++i) {
            subst_.emplace_back(fn->typeParams[i], args[i]);
        }
        lowerFunction(fn, function(idx));
        function(idx).exported = false;
    }
    subst_.clear();
    for (auto& [idx, f] : extra_) {
        mod_->functions.push_back(std::move(f));
    }
    return std::move(mod_);
}

std::unique_ptr<Module> lower(sema::Program& program, const SourceManager& sources, Diagnostics& diags) {
    return Lowerer(program, sources, diags).run();
}

} // namespace quartz::ir
