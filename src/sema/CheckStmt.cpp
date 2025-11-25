#include "sema/SemaImpl.hpp"

#include <format>

namespace quartz::sema {

namespace {

bool diverges(TypeId t) { return t == ir::kNever; }

} // namespace

void Checker::checkFunction(FnInfo* fn) {
    currentFn_ = fn;
    selfType_ = fn->owner;
    loopDepth_ = 0;
    scopes_.clear();
    pushScope();
    FnDecl* decl = fn->decl;
    std::size_t p = 0;
    if (fn->hasSelf) {
        fn->selfLocal = declareLocal(names_.intern("self"), fn->owner, false, decl->span);
        fn->paramLocals.push_back(fn->selfLocal);
        ++p;
    }
    for (Param& param : decl->params) {
        for (std::size_t j = 0; j < fn->paramLocals.size(); ++j) {
            if (fn->paramLocals[j]->name == param.name) {
                diags_.error(param.span, std::format("duplicate parameter '{}'", str(param.name)));
            }
        }
        param.local = declareLocal(param.name, fn->params[p++], false, param.span);
        fn->paramLocals.push_back(param.local);
    }
    TypeId bodyT = checkBlock(decl->body, ir::kError, false);
    decl->body->type = bodyT;
    if (fn->result != ir::kVoid && !diverges(bodyT) && fn->result != ir::kError) {
        diags_.error(decl->span, std::format("function '{}' may end without returning a '{}'", fn->name,
                                             typeName(fn->result)));
    }
    popScope();
    currentFn_ = nullptr;
    selfType_ = ir::kError;
}

TypeId Checker::checkBlock(BlockExpr* block, TypeId expected, bool wantValue) {
    pushScope();
    TypeId result = ir::kVoid;
    bool dead = false;
    for (std::size_t i = 0; i < block->stmts.size(); ++i) {
        Stmt* s = block->stmts[i];
        bool last = i + 1 == block->stmts.size();
        if (dead) {
            diags_.warning(s->span, "unreachable code");
            dead = false;
        }
        if (last && wantValue && s->kind == StmtKind::Expr) {
            Expr* e = static_cast<ExprStmt*>(s)->expr;
            result = check(e, expected);
            break;
        }
        checkStmt(s);
        auto* es = s->kind == StmtKind::Expr ? static_cast<ExprStmt*>(s) : nullptr;
        bool stmtDiverges = s->kind == StmtKind::Return || s->kind == StmtKind::Break ||
                            s->kind == StmtKind::Continue || (es != nullptr && diverges(es->expr->type));
        if (stmtDiverges) {
            result = ir::kNever;
            dead = !last;
        }
    }
    popScope();
    if (wantValue && result == ir::kVoid && expected != ir::kError && expected != ir::kVoid) {
        diags_.error(block->span, std::format("this block has no value; expected '{}'", typeName(expected)));
        return ir::kError;
    }
    block->type = result;
    return result;
}

void Checker::checkStmt(Stmt* s) {
    switch (s->kind) {
    case StmtKind::Expr: {
        Expr* e = static_cast<ExprStmt*>(s)->expr;
        if (e->kind == ExprKind::If) {
            e->type = checkIf(static_cast<IfExpr*>(e), ir::kError, false);
        } else if (e->kind == ExprKind::Match) {
            e->type = checkMatch(static_cast<MatchExpr*>(e), ir::kError, false);
        } else if (e->kind == ExprKind::Block) {
            e->type = checkBlock(static_cast<BlockExpr*>(e), ir::kError, false);
        } else {
            check(e, ir::kError);
        }
        return;
    }
    case StmtKind::Let: checkLet(static_cast<LetStmt*>(s)); return;
    case StmtKind::Assign: checkAssign(static_cast<AssignStmt*>(s)); return;
    case StmtKind::While: {
        auto* w = static_cast<WhileStmt*>(s);
        expectAssignable(checkValue(w->cond, ir::kBool), ir::kBool, w->cond->span);
        ++loopDepth_;
        checkBlock(w->body, ir::kError, false);
        --loopDepth_;
        return;
    }
    case StmtKind::For: checkFor(static_cast<ForStmt*>(s)); return;
    case StmtKind::Return: {
        auto* r = static_cast<ReturnStmt*>(s);
        TypeId want = currentFn_ != nullptr ? currentFn_->result : ir::kVoid;
        if (r->value == nullptr) {
            if (want != ir::kVoid && want != ir::kError) {
                diags_.error(s->span, std::format("missing return value of type '{}'", typeName(want)));
            }
            return;
        }
        if (want == ir::kVoid) {
            diags_.error(r->value->span, "this function does not return a value");
            check(r->value, ir::kError);
            return;
        }
        expectAssignable(checkValue(r->value, want), want, r->value->span);
        return;
    }
    case StmtKind::Break:
    case StmtKind::Continue:
        if (loopDepth_ == 0) {
            diags_.error(s->span, std::format("'{}' outside of a loop", s->kind == StmtKind::Break ? "break" : "continue"));
        }
        return;
    case StmtKind::Scope:
    case StmtKind::Every:
    case StmtKind::At: diags_.error(s->span, "'scope', 'every' and 'at' are not supported yet"); return;
    }
}

void Checker::checkLet(LetStmt* s) {
    TypeId declared = s->type != nullptr ? resolveType(s->type) : ir::kError;
    TypeId t = declared;
    if (s->init != nullptr) {
        TypeId it = checkValue(s->init, declared);
        if (declared != ir::kError) {
            expectAssignable(it, declared, s->init->span);
        } else {
            t = it;
            if (it == ir::kNull) {
                diags_.error(s->init->span, "cannot infer a type from 'null'; add a type annotation");
                t = ir::kError;
            } else if (it == ir::kNever) {
                t = ir::kError;
            }
        }
    } else if (s->type == nullptr) {
        diags_.error(s->nameSpan, "a variable without an initializer needs a type");
    } else {
        ir::TypeKind k = types_->kind(declared);
        if (k == ir::TypeKind::Struct || k == ir::TypeKind::Tuple || k == ir::TypeKind::Handle ||
            k == ir::TypeKind::State || k == ir::TypeKind::Enum || k == ir::TypeKind::Fn) {
            diags_.error(s->nameSpan, std::format("a variable of type '{}' needs an initializer", typeName(declared)));
        }
    }
    s->local = declareLocal(s->name, t, s->mutable_, s->nameSpan);
}

bool Checker::isPlace(Expr* e, bool& mutableOut) {
    switch (e->kind) {
    case ExprKind::Ident: {
        auto* id = static_cast<IdentExpr*>(e);
        mutableOut = id->ref == IdentRef::Local && id->local->mutable_;
        return id->ref == IdentRef::Local;
    }
    case ExprKind::SelfRef: mutableOut = false; return true;
    case ExprKind::Member: {
        auto* m = static_cast<MemberExpr*>(e);
        if (m->safe || m->isProperty || m->isVariant) {
            mutableOut = false;
            return false;
        }
        TypeId ot = m->object->type;
        if (types_->kind(ot) == ir::TypeKind::State) {
            mutableOut = true;
            return true;
        }
        return isPlace(m->object, mutableOut);
    }
    case ExprKind::Index: mutableOut = true; return true;
    default: mutableOut = false; return false;
    }
}

void Checker::checkAssign(AssignStmt* s) {
    TypeId tt = checkValue(s->target, ir::kError);
    bool mut = false;
    if (!isPlace(s->target, mut)) {
        if (s->target->kind == ExprKind::Member && static_cast<MemberExpr*>(s->target)->isProperty) {
            diags_.error(s->target->span, "host properties are read-only for now");
        } else {
            diags_.error(s->target->span, "cannot assign to this expression");
        }
    } else if (!mut) {
        diags_.error(s->target->span, "cannot assign to an immutable binding; declare it with 'var'");
    }
    if (s->op == AssignOp::Set) {
        expectAssignable(checkValue(s->value, tt), tt, s->value->span);
        return;
    }
    bool shift = s->op == AssignOp::Shl || s->op == AssignOp::Shr;
    TypeId vt = checkValue(s->value, shift ? ir::kI64 : tt);
    if (tt == ir::kError || vt == ir::kError) {
        return;
    }
    if (shift) {
        if (!types_->isInteger(tt) || !types_->isInteger(vt)) {
            diags_.error(s->span, "shift assignment needs integers");
        }
        return;
    }
    bool bitwise = s->op == AssignOp::BitAnd || s->op == AssignOp::BitOr || s->op == AssignOp::BitXor;
    bool ok = tt == vt && (bitwise ? (types_->isInteger(tt) || tt == ir::kBool)
                                   : (types_->isNumeric(tt) || (s->op == AssignOp::Add && tt == ir::kStr)));
    if (!ok) {
        diags_.error(s->span, std::format("compound assignment is not defined for '{}' and '{}'", typeName(tt),
                                          typeName(vt)));
    }
}

void Checker::checkFor(ForStmt* s) {
    TypeId elem = ir::kError;
    if (s->iterable->kind == ExprKind::Range) {
        auto* r = static_cast<RangeExpr*>(s->iterable);
        TypeId lt = ir::kError;
        TypeId ht = ir::kError;
        if (isUntypedLiteral(r->lo) && !isUntypedLiteral(r->hi)) {
            ht = checkValue(r->hi, ir::kError);
            lt = checkValue(r->lo, ht);
        } else {
            lt = checkValue(r->lo, ir::kError);
            ht = checkValue(r->hi, lt);
        }
        elem = unify(lt, ht, r->span);
        if (elem != ir::kError && !types_->isInteger(elem)) {
            diags_.error(r->span, std::format("a range needs integers, found '{}'", typeName(elem)));
            elem = ir::kError;
        }
        r->type = elem;
        if (s->indexName.valid()) {
            diags_.error(s->nameSpan, "a range loop has no index variable");
        }
    } else {
        TypeId it = checkValue(s->iterable, ir::kError);
        if (it != ir::kError) {
            if (types_->kind(it) == ir::TypeKind::List) {
                elem = types_->info(it).elem;
            } else {
                diags_.error(s->iterable->span, std::format("cannot iterate over '{}'", typeName(it)));
            }
        }
    }
    pushScope();
    if (s->indexName.valid()) {
        s->indexLocal = declareLocal(s->indexName, ir::kI64, false, s->nameSpan);
    }
    s->local = declareLocal(s->name, elem, false, s->nameSpan);
    ++loopDepth_;
    checkBlock(s->body, ir::kError, false);
    --loopDepth_;
    popScope();
}

void Checker::checkPattern(Pattern* p, TypeId subject) {
    const ir::TypeInfo& info = types_->info(subject);
    switch (p->kind) {
    case PatternKind::Wildcard: return;
    case PatternKind::Binding:
        if (info.kind == ir::TypeKind::Enum) {
            for (std::uint32_t v = 0; v < info.variants.size(); ++v) {
                if (info.variants[v].name == str(p->name)) {
                    p->kind = PatternKind::Variant;
                    p->variantIndex = v;
                    if (!info.variants[v].payload.empty()) {
                        diags_.error(p->span, std::format("variant '{}' has a payload; match it with '{}(...)'",
                                                          str(p->name), str(p->name)));
                    }
                    return;
                }
            }
        }
        p->local = declareLocal(p->name, subject, false, p->span);
        return;
    case PatternKind::Literal:
    case PatternKind::Range: {
        Expr* lits[] = {p->literal, p->rangeHi};
        for (Expr* lit : lits) {
            if (lit == nullptr) {
                continue;
            }
            bool literal = lit->kind == ExprKind::IntLit || lit->kind == ExprKind::CharLit ||
                           lit->kind == ExprKind::StringLit || lit->kind == ExprKind::BoolLit ||
                           (lit->kind == ExprKind::Unary && static_cast<UnaryExpr*>(lit)->operand->kind ==
                                                                ExprKind::IntLit);
            if (!literal) {
                diags_.error(lit->span, "patterns must be literals");
            }
            expectAssignable(checkValue(lit, subject), subject, lit->span);
        }
        if (p->kind == PatternKind::Range && !types_->isInteger(subject) && subject != ir::kChar &&
            subject != ir::kError) {
            diags_.error(p->span, "range patterns need an integer or char subject");
        }
        return;
    }
    case PatternKind::Variant: {
        if (info.kind != ir::TypeKind::Enum) {
            if (subject != ir::kError) {
                diags_.error(p->span, std::format("variant pattern on a value of type '{}'", typeName(subject)));
            }
            return;
        }
        if (p->path.size() > 2 || (p->path.size() == 2 && str(p->path[0]) != info.name)) {
            diags_.error(p->span, std::format("pattern does not belong to '{}'", info.name));
            return;
        }
        for (std::uint32_t v = 0; v < info.variants.size(); ++v) {
            if (info.variants[v].name == str(p->name)) {
                p->variantIndex = v;
                const std::vector<TypeId>& payload = info.variants[v].payload;
                if (p->sub.size() != payload.size()) {
                    diags_.error(p->span, std::format("variant '{}' has {} field(s), the pattern has {}",
                                                      str(p->name), payload.size(), p->sub.size()));
                    return;
                }
                for (std::size_t i = 0; i < payload.size(); ++i) {
                    if (p->sub[i]->kind == PatternKind::Variant) {
                        diags_.error(p->sub[i]->span, "nested variant patterns are not supported yet");
                        continue;
                    }
                    checkPattern(p->sub[i], payload[i]);
                }
                return;
            }
        }
        diags_.error(p->span, std::format("'{}' has no variant '{}'", info.name, str(p->name)));
        return;
    }
    }
}

TypeId Checker::checkMatch(MatchExpr* m, TypeId expected, bool wantValue) {
    TypeId subject = checkValue(m->subject, ir::kError);
    const ir::TypeInfo& sinfo = types_->info(subject);
    bool scalarOk = types_->isInteger(subject) || subject == ir::kChar || subject == ir::kStr ||
                    subject == ir::kBool || sinfo.kind == ir::TypeKind::Enum || subject == ir::kError;
    if (!scalarOk) {
        diags_.error(m->subject->span, std::format("cannot match on '{}'", typeName(subject)));
    }
    TypeId result = ir::kNever;
    bool catchAll = false;
    std::vector<bool> covered(sinfo.kind == ir::TypeKind::Enum ? sinfo.variants.size() : 0, false);
    bool sawTrue = false;
    bool sawFalse = false;
    for (MatchArm& arm : m->arms) {
        pushScope();
        checkPattern(arm.pattern, subject);
        if (arm.guard != nullptr) {
            expectAssignable(checkValue(arm.guard, ir::kBool), ir::kBool, arm.guard->span);
        } else {
            PatternKind k = arm.pattern->kind;
            if (k == PatternKind::Wildcard || k == PatternKind::Binding) {
                catchAll = true;
            } else if (k == PatternKind::Variant && arm.pattern->variantIndex < covered.size()) {
                bool all = true;
                for (Pattern* sub : arm.pattern->sub) {
                    all = all && (sub->kind == PatternKind::Wildcard || sub->kind == PatternKind::Binding);
                }
                if (all) {
                    covered[arm.pattern->variantIndex] = true;
                }
            } else if (k == PatternKind::Literal && arm.pattern->literal->kind == ExprKind::BoolLit) {
                (static_cast<BoolLitExpr*>(arm.pattern->literal)->value ? sawTrue : sawFalse) = true;
            }
        }
        TypeId t = ir::kVoid;
        if (wantValue) {
            t = checkValue(arm.body, expected);
            result = unify(result, t, arm.body->span);
        } else if (arm.body->kind == ExprKind::Block) {
            t = checkBlock(static_cast<BlockExpr*>(arm.body), ir::kError, false);
            arm.body->type = t;
        } else if (arm.body->kind == ExprKind::If) {
            t = checkIf(static_cast<IfExpr*>(arm.body), ir::kError, false);
            arm.body->type = t;
        } else {
            t = check(arm.body, ir::kError);
        }
        if (!wantValue && t != ir::kNever) {
            result = ir::kVoid;
        }
        popScope();
    }
    bool exhaustive = catchAll || (subject == ir::kBool && sawTrue && sawFalse);
    if (!covered.empty() && !exhaustive) {
        exhaustive = true;
        std::string missing;
        for (std::size_t v = 0; v < covered.size(); ++v) {
            if (!covered[v]) {
                exhaustive = false;
                missing += (missing.empty() ? "" : ", ") + sinfo.variants[v].name;
            }
        }
        if (!exhaustive) {
            diags_.error(m->span, std::format("match is not exhaustive; missing: {}", missing));
            exhaustive = true;
        }
    }
    if (!exhaustive && subject != ir::kError) {
        diags_.error(m->span, "match is not exhaustive; add a '_' arm");
    }
    if (m->arms.empty()) {
        return wantValue ? ir::kError : ir::kVoid;
    }
    return result;
}

} // namespace quartz::sema
