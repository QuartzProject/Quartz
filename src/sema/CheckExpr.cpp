#include "sema/SemaImpl.hpp"

#include <format>
#include <limits>

namespace quartz::sema {

bool Checker::isUntypedLiteral(const Expr* e) {
    if (e->kind == ExprKind::IntLit || e->kind == ExprKind::FloatLit || e->kind == ExprKind::NullLit) {
        return true;
    }
    if (e->kind == ExprKind::Unary) {
        return isUntypedLiteral(static_cast<const UnaryExpr*>(e)->operand);
    }
    return false;
}

bool Checker::assignable(TypeId from, TypeId to) const {
    if (from == to || from == ir::kError || to == ir::kError || from == ir::kNever) {
        return true;
    }
    if (types_->kind(to) == ir::TypeKind::Nullable) {
        return from == ir::kNull || assignable(from, types_->info(to).elem);
    }
    return false;
}

void Checker::expectAssignable(TypeId from, TypeId to, Span span) {
    if (!assignable(from, to)) {
        diags_.error(span, std::format("expected '{}', found '{}'", typeName(to), typeName(from)));
    }
}

TypeId Checker::unify(TypeId a, TypeId b, Span span) {
    if (a == ir::kNever) {
        return b;
    }
    if (b == ir::kNever || a == b) {
        return a;
    }
    if (a == ir::kError || b == ir::kError) {
        return ir::kError;
    }
    if (assignable(a, b)) {
        return b;
    }
    if (assignable(b, a)) {
        return a;
    }
    if (a == ir::kNull) {
        return types_->nullable(b);
    }
    if (b == ir::kNull) {
        return types_->nullable(a);
    }
    diags_.error(span, std::format("mismatched types '{}' and '{}'", typeName(a), typeName(b)));
    return ir::kError;
}

TypeId Checker::check(Expr* e, TypeId expected) {
    TypeId t = checkInner(e, expected);
    e->type = t;
    if (!loopWatch_.empty()) {
        noteAllocation(e, t);
    }
    return t;
}

void Checker::noteAllocation(const Expr* e, TypeId t) {
    bool refResult =
        t == ir::kStr || t == ir::kBytes ||
        (t < types_->size() && (types_->kind(t) == ir::TypeKind::List || types_->kind(t) == ir::TypeKind::Map));
    bool allocates = e->kind == ExprKind::Interp || e->kind == ExprKind::ListLit || e->kind == ExprKind::ListRepeat ||
                     (e->kind == ExprKind::Binary && t == ir::kStr) || (e->kind == ExprKind::Call && refResult);
    if (!allocates) {
        return;
    }
    for (LoopWatch& w : loopWatch_) {
        if (w.scopeLevel == scopeLevel_ && !w.allocates) {
            w.allocates = true;
            w.firstAlloc = e->span;
        }
    }
}

TypeId Checker::checkValue(Expr* e, TypeId expected) {
    TypeId t = check(e, expected);
    if (t == ir::kVoid) {
        diags_.error(e->span, "this expression has no value");
        e->type = ir::kError;
        return ir::kError;
    }
    return t;
}

namespace {

bool intFits(std::uint64_t v, TypeId t, bool negative) {
    switch (t) {
    case ir::kI8: return negative ? v <= 128 : v <= 127;
    case ir::kI16: return negative ? v <= 32768 : v <= 32767;
    case ir::kI32: return negative ? v <= 2147483648ULL : v <= 2147483647ULL;
    case ir::kI64: return negative ? v <= 9223372036854775808ULL : v <= 9223372036854775807ULL;
    case ir::kU8: return !negative && v <= 255;
    case ir::kU16: return !negative && v <= 65535;
    case ir::kU32: return !negative && v <= 4294967295ULL;
    case ir::kU64: return !negative || v == 0;
    default: return true;
    }
}

} // namespace

TypeId Checker::checkInner(Expr* e, TypeId expected) {
    TypeId want = expected;
    if (want != ir::kError && types_->kind(want) == ir::TypeKind::Nullable) {
        want = types_->info(want).elem;
    }
    switch (e->kind) {
    case ExprKind::IntLit: {
        auto* lit = static_cast<IntLitExpr*>(e);
        TypeId t = types_->isNumeric(want) ? want : ir::kI64;
        if (types_->isInteger(t) && !intFits(lit->value, t, false)) {
            diags_.error(e->span, std::format("literal does not fit in '{}'", typeName(t)));
        }
        return t;
    }
    case ExprKind::FloatLit: return types_->isFloat(want) ? want : ir::kF64;
    case ExprKind::BoolLit: return ir::kBool;
    case ExprKind::CharLit: return ir::kChar;
    case ExprKind::StringLit: return ir::kStr;
    case ExprKind::NullLit:
        return expected != ir::kError && types_->kind(expected) == ir::TypeKind::Nullable ? expected : ir::kNull;
    case ExprKind::Interp: return checkInterp(static_cast<InterpExpr*>(e));
    case ExprKind::Ident: return checkIdent(static_cast<IdentExpr*>(e), expected);
    case ExprKind::SelfRef: {
        auto* s = static_cast<SelfExpr*>(e);
        if (currentFn_ == nullptr || currentFn_->selfLocal == nullptr) {
            diags_.error(e->span, "'self' is only available in methods");
            return ir::kError;
        }
        s->local = currentFn_->selfLocal;
        if (s->local->lambdaDepth < lambdas_.size()) {
            noteCapture(s->local);
        }
        return s->local->type;
    }
    case ExprKind::Unary: return checkUnary(static_cast<UnaryExpr*>(e), expected);
    case ExprKind::Binary: return checkBinary(static_cast<BinaryExpr*>(e), expected);
    case ExprKind::Call: return checkCall(static_cast<CallExpr*>(e), expected);
    case ExprKind::Member: return checkMember(static_cast<MemberExpr*>(e), expected);
    case ExprKind::Index: return checkIndex(static_cast<IndexExpr*>(e));
    case ExprKind::Propagate: {
        auto* p = static_cast<PropagateExpr*>(e);
        TypeId inner = checkValue(p->inner, ir::kError);
        if (inner == ir::kError) {
            return inner;
        }
        if (types_->isResult(inner)) {
            TypeId fr = currentFn_ != nullptr ? currentFn_->result : ir::kError;
            if (!types_->isResult(fr)) {
                diags_.error(e->span, "'?' on a Result can only be used in a function that returns a Result");
            } else if (types_->info(fr).variants[1].payload[0] != types_->info(inner).variants[1].payload[0]) {
                diags_.error(e->span, std::format("error type '{}' does not match the function's '{}'",
                                                  typeName(types_->info(inner).variants[1].payload[0]),
                                                  typeName(types_->info(fr).variants[1].payload[0])));
            }
            return types_->info(inner).variants[0].payload[0];
        }
        if (types_->kind(inner) != ir::TypeKind::Nullable) {
            diags_.error(e->span, std::format("'?' needs a nullable value or a Result, found '{}'", typeName(inner)));
            return ir::kError;
        }
        if (currentFn_ == nullptr || types_->kind(currentFn_->result) != ir::TypeKind::Nullable) {
            diags_.error(e->span, "'?' can only be used in a function that returns a nullable type");
        }
        return types_->info(inner).elem;
    }
    case ExprKind::Range:
        diags_.error(e->span, "a range can only be used in a 'for' loop or a match pattern");
        return ir::kError;
    case ExprKind::ListLit: {
        auto* l = static_cast<ListLitExpr*>(e);
        TypeId elem = types_->kind(want) == ir::TypeKind::List ? types_->info(want).elem : ir::kError;
        if (l->elems.empty()) {
            if (elem == ir::kError) {
                diags_.error(e->span, "cannot infer the type of an empty list; add a type annotation");
                return ir::kError;
            }
            return want;
        }
        TypeId first = checkValue(l->elems[0], elem);
        if (elem == ir::kError) {
            elem = first;
        } else {
            expectAssignable(first, elem, l->elems[0]->span);
        }
        for (std::size_t i = 1; i < l->elems.size(); ++i) {
            expectAssignable(checkValue(l->elems[i], elem), elem, l->elems[i]->span);
        }
        return elem == ir::kNull ? ir::kError : types_->list(elem);
    }
    case ExprKind::ListRepeat: {
        auto* r = static_cast<ListRepeatExpr*>(e);
        TypeId elem = types_->kind(want) == ir::TypeKind::List ? types_->info(want).elem : ir::kError;
        TypeId vt = checkValue(r->value, elem);
        if (elem != ir::kError) {
            expectAssignable(vt, elem, r->value->span);
            vt = elem;
        }
        TypeId ct = checkValue(r->count, ir::kI64);
        if (ct != ir::kError && !types_->isInteger(ct)) {
            diags_.error(r->count->span, "the list length must be an integer");
        }
        return vt == ir::kError ? ir::kError : types_->list(vt);
    }
    case ExprKind::TupleLit: {
        auto* t = static_cast<TupleLitExpr*>(e);
        if (t->elems.empty()) {
            return ir::kVoid;
        }
        const ir::TypeInfo* hint = types_->kind(want) == ir::TypeKind::Tuple ? &types_->info(want) : nullptr;
        std::vector<TypeId> elems;
        for (std::size_t i = 0; i < t->elems.size(); ++i) {
            TypeId h = hint != nullptr && i < hint->elems.size() ? hint->elems[i] : ir::kError;
            elems.push_back(checkValue(t->elems[i], h));
        }
        return types_->tuple(elems);
    }
    case ExprKind::Lambda:
        return checkLambda(static_cast<LambdaExpr*>(e), expected);
    case ExprKind::Cast: return checkCast(static_cast<CastExpr*>(e));
    case ExprKind::Is:
        return checkIs(static_cast<IsExpr*>(e));
    case ExprKind::Wait: return checkWait(static_cast<WaitExpr*>(e)->value, e->span);
    case ExprKind::If: return checkIf(static_cast<IfExpr*>(e), expected, true);
    case ExprKind::Match: return checkMatch(static_cast<MatchExpr*>(e), expected, true);
    case ExprKind::Block: return checkBlock(static_cast<BlockExpr*>(e), expected, true);
    }
    return ir::kError;
}

TypeId Checker::checkIdent(IdentExpr* e, TypeId expected) {
    if (Local* l = findLocal(e->name)) {
        e->ref = IdentRef::Local;
        e->local = l;
        if (l->lambdaDepth < lambdas_.size()) {
            if (l->stateView && holdsMutable(l->type)) {
                diags_.error(e->span, std::format("a lambda cannot capture '{}', a list or map of the state: after a "
                                                  "'wait' it would be a copy; use it through the state",
                                                  str(e->name)));
            }
            noteCapture(l);
        }
        if (atDepth_ > 0 && types_->hasRefs(l->type) && l->type != ir::kStr) {
            bool outer = true;
            for (std::size_t i = atScopeBase_; i < scopes_.size(); ++i) {
                auto found = scopes_[i].find(e->name);
                if (found != scopes_[i].end() && found->second == l) {
                    outer = false;
                }
            }
            if (outer) {
                diags_.error(e->span, std::format("'{}' cannot be used inside 'at': only copyable values cross contexts",
                                                  str(e->name)));
            }
        }
        return l->type;
    }
    if (GlobalInfo* g = findGlobal(e->name)) {
        e->ref = IdentRef::Const;
        e->global = g;
        if (g->type == ir::kError) {
            if (g->checking) {
                diags_.error(e->span, std::format("constant '{}' depends on itself", str(e->name)));
                return ir::kError;
            }
            g->checking = true;
            auto saved = std::move(scopes_);
            scopes_.clear();
            std::uint32_t savedFile = currentFile_;
            currentFile_ = g->decl->span.file;
            TypeId declared = g->decl->type != nullptr ? resolveType(g->decl->type) : ir::kError;
            g->type = checkValue(g->decl->value, declared);
            if (declared != ir::kError) {
                expectAssignable(g->type, declared, g->decl->value->span);
                g->type = declared;
            }
            scopes_ = std::move(saved);
            currentFile_ = savedFile;
        }
        return g->type;
    }
    if (FnInfo* fn = findFunction(e->name)) {
        return checkFunctionValue(e, fn, expected);
    }
    if (lookupTypeName(e->name, e->span, false) != ir::kError) {
        diags_.error(e->span, std::format("'{}' is a type, not a value", str(e->name)));
        return ir::kError;
    }
    diags_.error(e->span, std::format("unknown name '{}'", str(e->name)));
    return ir::kError;
}

TypeId Checker::checkUnary(UnaryExpr* e, TypeId expected) {
    if (e->op == UnaryOp::Neg && e->operand->kind == ExprKind::IntLit) {
        auto* lit = static_cast<IntLitExpr*>(e->operand);
        TypeId want = expected != ir::kError && types_->kind(expected) == ir::TypeKind::Nullable
                          ? types_->info(expected).elem
                          : expected;
        TypeId t = types_->isNumeric(want) ? want : ir::kI64;
        if (types_->isInteger(t) && !intFits(lit->value, t, true)) {
            diags_.error(e->span, std::format("literal does not fit in '{}'", typeName(t)));
        }
        e->operand->type = t;
        return t;
    }
    TypeId t = checkValue(e->operand, expected);
    if (t == ir::kError) {
        return t;
    }
    switch (e->op) {
    case UnaryOp::Neg:
        if (!types_->isNumeric(t)) {
            diags_.error(e->span, std::format("cannot negate '{}'", typeName(t)));
            return ir::kError;
        }
        return t;
    case UnaryOp::Not:
        if (t != ir::kBool) {
            diags_.error(e->span, std::format("'!' needs a bool, found '{}'", typeName(t)));
            return ir::kError;
        }
        return t;
    case UnaryOp::BitNot:
        if (!types_->isInteger(t)) {
            diags_.error(e->span, std::format("'~' needs an integer, found '{}'", typeName(t)));
            return ir::kError;
        }
        return t;
    }
    return ir::kError;
}

TypeId Checker::checkBinary(BinaryExpr* e, TypeId expected) {
    BinaryOp op = e->op;
    if (op == BinaryOp::And || op == BinaryOp::Or) {
        expectAssignable(checkValue(e->lhs, ir::kBool), ir::kBool, e->lhs->span);
        expectAssignable(checkValue(e->rhs, ir::kBool), ir::kBool, e->rhs->span);
        return ir::kBool;
    }
    if (op == BinaryOp::Coalesce) {
        TypeId hint = expected != ir::kError ? types_->nullable(expected) : ir::kError;
        TypeId lt = checkValue(e->lhs, hint);
        if (lt == ir::kError) {
            check(e->rhs, expected);
            return ir::kError;
        }
        if (types_->kind(lt) != ir::TypeKind::Nullable) {
            diags_.error(e->lhs->span, std::format("'?" "?' needs a nullable value on the left, found '{}'", typeName(lt)));
            check(e->rhs, expected);
            return lt;
        }
        TypeId inner = types_->info(lt).elem;
        TypeId rt = checkValue(e->rhs, expected != ir::kError ? expected : inner);
        if (assignable(rt, inner)) {
            return types_->kind(rt) == ir::TypeKind::Nullable || rt == ir::kNull ? lt : inner;
        }
        expectAssignable(rt, lt, e->rhs->span);
        return lt;
    }
    bool shift = op == BinaryOp::Shl || op == BinaryOp::Shr;
    bool compare = op >= BinaryOp::Eq && op <= BinaryOp::Ge;
    TypeId hint = compare ? ir::kError : expected;
    if (hint != ir::kError && types_->kind(hint) == ir::TypeKind::Nullable) {
        hint = types_->info(hint).elem;
    }
    TypeId lt = ir::kError;
    TypeId rt = ir::kError;
    if (shift) {
        lt = checkValue(e->lhs, hint);
        rt = checkValue(e->rhs, ir::kI64);
        if (lt != ir::kError && !types_->isInteger(lt)) {
            diags_.error(e->lhs->span, "shifts need an integer on the left");
            return ir::kError;
        }
        if (rt != ir::kError && !types_->isInteger(rt)) {
            diags_.error(e->rhs->span, "the shift amount must be an integer");
        }
        return lt;
    }
    if (isUntypedLiteral(e->lhs) && !isUntypedLiteral(e->rhs)) {
        rt = checkValue(e->rhs, hint);
        lt = checkValue(e->lhs, rt);
    } else {
        lt = checkValue(e->lhs, hint);
        rt = checkValue(e->rhs, lt);
    }
    if (lt == ir::kError || rt == ir::kError) {
        return compare ? ir::kBool : ir::kError;
    }
    if (compare) {
        bool eq = op == BinaryOp::Eq || op == BinaryOp::Ne;
        TypeId t = lt;
        if (lt != rt) {
            if (eq && (assignable(rt, lt) || assignable(lt, rt))) {
                t = assignable(rt, lt) ? lt : rt;
            } else {
                diags_.error(e->span, std::format("cannot compare '{}' with '{}'", typeName(lt), typeName(rt)));
                return ir::kBool;
            }
        }
        bool withNull = (lt == ir::kNull || rt == ir::kNull) && types_->kind(t) == ir::TypeKind::Nullable;
        TraitInfo* need = langTrait(eq ? LangTrait::Eq : LangTrait::Ord);
        if (!withNull && !implements(t, need)) {
            diags_.error(e->span, std::format("operator '{}' is not defined for '{}'; it needs {}", binaryOpText(op),
                                              typeName(t), need->name));
        }
        return ir::kBool;
    }
    if (lt != rt) {
        diags_.error(e->span, std::format("mismatched types '{}' and '{}' for '{}'; use 'as' to convert",
                                          typeName(lt), typeName(rt), binaryOpText(op)));
        return ir::kError;
    }
    if (op == BinaryOp::Add && lt == ir::kStr) {
        return ir::kStr;
    }
    bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
    if (bitwise && (types_->isInteger(lt) || lt == ir::kBool)) {
        return lt;
    }
    if (!bitwise && types_->isNumeric(lt)) {
        return lt;
    }
    diags_.error(e->span, std::format("operator '{}' is not defined for '{}'", binaryOpText(op), typeName(lt)));
    return ir::kError;
}

TypeId Checker::checkIf(IfExpr* e, TypeId expected, bool wantValue) {
    pushScope();
    if (e->bindValue != nullptr) {
        TypeId bt = checkValue(e->bindValue, ir::kError);
        TypeId inner = ir::kError;
        if (bt != ir::kError) {
            if (types_->kind(bt) != ir::TypeKind::Nullable) {
                diags_.error(e->bindValue->span, std::format("'if let' needs a nullable value, found '{}'", typeName(bt)));
            } else {
                inner = types_->info(bt).elem;
            }
        }
        e->bindLocal = declareLocal(e->bindName, inner, false, e->bindValue->span);
    } else {
        allowIsBindings(e->cond);
        expectAssignable(checkValue(e->cond, ir::kBool), ir::kBool, e->cond->span);
    }
    TypeId thenT = checkBlock(e->then, expected, wantValue);
    e->then->type = thenT;
    popScope();
    if (e->otherwise == nullptr) {
        if (wantValue && thenT != ir::kVoid && thenT != ir::kNever) {
            diags_.error(e->span, "an 'if' used as a value needs an 'else' branch");
        }
        return ir::kVoid;
    }
    TypeId elseT = ir::kVoid;
    if (e->otherwise->kind == ExprKind::If) {
        elseT = checkIf(static_cast<IfExpr*>(e->otherwise), expected, wantValue);
    } else {
        elseT = checkBlock(static_cast<BlockExpr*>(e->otherwise), expected, wantValue);
    }
    e->otherwise->type = elseT;
    if (!wantValue) {
        return thenT == ir::kNever && elseT == ir::kNever ? ir::kNever : ir::kVoid;
    }
    return unify(thenT, elseT, e->span);
}

TypeId Checker::checkCast(CastExpr* e) {
    TypeId from = checkValue(e->value, ir::kError);
    TypeId to = resolveType(e->target);
    if (from == ir::kError || to == ir::kError) {
        return to;
    }
    bool ok = from == to || (types_->isNumeric(from) && types_->isNumeric(to)) ||
              (from == ir::kChar && types_->isInteger(to)) || (from == ir::kBool && types_->isInteger(to)) ||
              (from == ir::kU8 && to == ir::kChar);
    if (!ok) {
        diags_.error(e->span, std::format("cannot convert '{}' to '{}'", typeName(from), typeName(to)));
    }
    return to;
}

TypeId Checker::checkIndex(IndexExpr* e) {
    TypeId ot = checkValue(e->object, ir::kError);
    if (ot == ir::kError) {
        check(e->index, ir::kError);
        return ot;
    }
    const ir::TypeInfo& info = types_->info(ot);
    if (info.kind == ir::TypeKind::List) {
        TypeId it = checkValue(e->index, ir::kI64);
        if (it != ir::kError && !types_->isInteger(it)) {
            diags_.error(e->index->span, std::format("list index must be an integer, found '{}'", typeName(it)));
        }
        return info.elem;
    }
    if (info.kind == ir::TypeKind::Map) {
        expectAssignable(checkValue(e->index, info.key), info.key, e->index->span);
        return info.elem;
    }
    diags_.error(e->span, std::format("'{}' cannot be indexed", typeName(ot)));
    check(e->index, ir::kError);
    return ir::kError;
}

TypeId Checker::checkInterp(InterpExpr* e) {
    for (Expr* part : e->exprs) {
        TypeId t = checkValue(part, ir::kError);
        if (t == ir::kError) {
            continue;
        }
        if (!implements(t, langTrait(LangTrait::Show))) {
            diags_.error(part->span, std::format("cannot interpolate a value of type '{}'; it does not implement Show",
                                                 typeName(t)));
        }
    }
    return ir::kStr;
}

TypeId Checker::checkWait(Expr* operand, Span span) {
    for (LoopWatch& w : loopWatch_) {
        w.waits = true;
    }
    if (currentFn_ == nullptr || !currentFn_->isSuspend) {
        diags_.error(span, "'wait' can only be used in a suspend function");
    }
    TypeId t = checkValue(operand, ir::kError);
    if (t == ir::kError) {
        return ir::kVoid;
    }
    auto it = awaitable_.find(t);
    if (it == awaitable_.end()) {
        diags_.error(operand->span, std::format("cannot wait on '{}'; the host must declare it @awaitable", typeName(t)));
        return ir::kVoid;
    }
    return it->second;
}

Local* Checker::declareLocal(Symbol name, TypeId type, bool mutable_, Span span) {
    auto* l = prog_->localStorage.emplace_back(std::make_unique<Local>()).get();
    l->name = name;
    l->type = type;
    l->mutable_ = mutable_;
    l->span = span;
    l->lambdaDepth = static_cast<std::uint32_t>(lambdas_.size());
    if (!scopes_.empty() && name.valid() && str(name) != "_") {
        scopes_.back()[name] = l;
    }
    return l;
}

Local* Checker::findLocal(Symbol name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return found->second;
        }
    }
    return nullptr;
}

} // namespace quartz::sema
