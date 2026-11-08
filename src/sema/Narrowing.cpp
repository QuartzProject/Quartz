#include "sema/SemaImpl.hpp"

#include <algorithm>

namespace quartz::sema {

namespace {

bool writes(const Expr* e, Symbol name);

bool writes(const Stmt* s, Symbol name);

bool writesAny(std::span<Expr* const> es, Symbol name) {
    return std::any_of(es.begin(), es.end(), [&](const Expr* e) { return writes(e, name); });
}

// The variable an assignment changes: `x`, `x.a`, `x[i]` and their chains all count as writing `x`.
bool rootNamed(const Expr* target, Symbol name) {
    while (true) {
        switch (target->kind) {
        case ExprKind::Ident:
            return static_cast<const IdentExpr*>(target)->name == name;
        case ExprKind::Member:
            target = static_cast<const MemberExpr*>(target)->object;
            break;
        case ExprKind::Index:
            target = static_cast<const IndexExpr*>(target)->object;
            break;
        default:
            return false;
        }
    }
}

bool writes(const Expr* e, Symbol name) {
    if (e == nullptr) {
        return false;
    }
    switch (e->kind) {
    case ExprKind::Interp:
        return writesAny(static_cast<const InterpExpr*>(e)->exprs, name);
    case ExprKind::Unary:
        return writes(static_cast<const UnaryExpr*>(e)->operand, name);
    case ExprKind::Binary: {
        const auto* b = static_cast<const BinaryExpr*>(e);
        return writes(b->lhs, name) || writes(b->rhs, name);
    }
    case ExprKind::Call: {
        const auto* c = static_cast<const CallExpr*>(e);
        return writes(c->callee, name) ||
               std::any_of(c->args.begin(), c->args.end(), [&](const Arg& a) { return writes(a.value, name); });
    }
    case ExprKind::Member:
        return writes(static_cast<const MemberExpr*>(e)->object, name);
    case ExprKind::Index: {
        const auto* ix = static_cast<const IndexExpr*>(e);
        return writes(ix->object, name) || writes(ix->index, name);
    }
    case ExprKind::Propagate:
        return writes(static_cast<const PropagateExpr*>(e)->inner, name);
    case ExprKind::Range: {
        const auto* r = static_cast<const RangeExpr*>(e);
        return writes(r->lo, name) || writes(r->hi, name);
    }
    case ExprKind::ListLit:
        return writesAny(static_cast<const ListLitExpr*>(e)->elems, name);
    case ExprKind::TupleLit:
        return writesAny(static_cast<const TupleLitExpr*>(e)->elems, name);
    case ExprKind::ListRepeat: {
        const auto* r = static_cast<const ListRepeatExpr*>(e);
        return writes(r->value, name) || writes(r->count, name);
    }
    case ExprKind::Lambda:
        return writes(static_cast<const LambdaExpr*>(e)->body, name);
    case ExprKind::Cast:
        return writes(static_cast<const CastExpr*>(e)->value, name);
    case ExprKind::Is:
        return writes(static_cast<const IsExpr*>(e)->value, name);
    case ExprKind::With: {
        const auto* w = static_cast<const WithExpr*>(e);
        return writes(w->value, name) ||
               std::any_of(w->fields.begin(), w->fields.end(), [&](const Arg& a) { return writes(a.value, name); });
    }
    case ExprKind::Wait:
        return writes(static_cast<const WaitExpr*>(e)->value, name);
    case ExprKind::If: {
        const auto* i = static_cast<const IfExpr*>(e);
        return writes(i->cond, name) || writes(i->bindValue, name) || writes(i->then, name) ||
               writes(i->otherwise, name);
    }
    case ExprKind::Match: {
        const auto* m = static_cast<const MatchExpr*>(e);
        return writes(m->subject, name) || std::any_of(m->arms.begin(), m->arms.end(), [&](const MatchArm& a) {
                   return writes(a.guard, name) || writes(a.body, name);
               });
    }
    case ExprKind::Block: {
        const auto* b = static_cast<const BlockExpr*>(e);
        return std::any_of(b->stmts.begin(), b->stmts.end(), [&](const Stmt* s) { return writes(s, name); });
    }
    case ExprKind::Jump:
        return writes(static_cast<const JumpExpr*>(e)->stmt, name);
    default:
        return false;
    }
}

bool writes(const Stmt* s, Symbol name) {
    if (s == nullptr) {
        return false;
    }
    switch (s->kind) {
    case StmtKind::Expr:
        return writes(static_cast<const ExprStmt*>(s)->expr, name);
    case StmtKind::Let:
        return writes(static_cast<const LetStmt*>(s)->init, name);
    case StmtKind::Assign: {
        const auto* a = static_cast<const AssignStmt*>(s);
        return rootNamed(a->target, name) || writes(a->target, name) || writes(a->value, name);
    }
    case StmtKind::While: {
        const auto* w = static_cast<const WhileStmt*>(s);
        return writes(w->cond, name) || writes(w->body, name);
    }
    case StmtKind::For: {
        const auto* f = static_cast<const ForStmt*>(s);
        return writes(f->iterable, name) || writes(f->body, name);
    }
    case StmtKind::Return:
        return writes(static_cast<const ReturnStmt*>(s)->value, name);
    case StmtKind::Scope:
        return writes(static_cast<const ScopeStmt*>(s)->body, name);
    case StmtKind::Every: {
        const auto* ev = static_cast<const EveryStmt*>(s);
        return writes(ev->interval, name) || writes(ev->body, name);
    }
    case StmtKind::At: {
        const auto* at = static_cast<const AtStmt*>(s);
        return writes(at->context, name) || writes(at->body, name);
    }
    default:
        return false;
    }
}

const IdentExpr* nullableLocal(const Expr* e) {
    if (e->kind != ExprKind::Ident) {
        return nullptr;
    }
    const auto* id = static_cast<const IdentExpr*>(e);
    return id->ref == IdentRef::Local && id->local != nullptr ? id : nullptr;
}

} // namespace

// The locals a condition proves null when true: `x is null`, `x == null`, `null == x`, joined with `||`.
void Checker::nullTests(const Expr* cond, std::vector<Local*>& out) const {
    if (cond->kind == ExprKind::Binary) {
        const auto* b = static_cast<const BinaryExpr*>(cond);
        if (b->op == BinaryOp::Or) {
            nullTests(b->lhs, out);
            nullTests(b->rhs, out);
            return;
        }
        if (b->op != BinaryOp::Eq) {
            return;
        }
        const Expr* other = b->lhs->kind == ExprKind::NullLit   ? b->rhs
                            : b->rhs->kind == ExprKind::NullLit ? b->lhs
                                                                : nullptr;
        if (const IdentExpr* id = other != nullptr ? nullableLocal(other) : nullptr) {
            out.push_back(id->local);
        }
        return;
    }
    if (cond->kind == ExprKind::Is) {
        const auto* is = static_cast<const IsExpr*>(cond);
        const Pattern* p = is->pattern;
        if (p->kind == PatternKind::Literal && p->literal->kind == ExprKind::NullLit) {
            if (const IdentExpr* id = nullableLocal(is->value)) {
                out.push_back(id->local);
            }
        }
    }
}

// Like Kotlin's smart casts, but only where one pass proves it: the variable cannot change in the rest of the block.
void Checker::narrowAfterExit(const IfExpr* e, std::span<Stmt* const> rest) {
    if (e->cond == nullptr || e->then->type != ir::kNever || rest.empty()) {
        return;
    }
    std::vector<Local*> tested;
    nullTests(e->cond, tested);
    for (Local* l : tested) {
        if (types_->kind(l->type) != ir::TypeKind::Nullable || narrowed(l)) {
            continue;
        }
        if (l->mutable_ && std::any_of(rest.begin(), rest.end(), [&](const Stmt* s) { return writes(s, l->name); })) {
            continue;
        }
        narrowed_.push_back(l);
    }
}

bool Checker::narrowed(const Local* l) const {
    return std::find(narrowed_.begin(), narrowed_.end(), l) != narrowed_.end();
}

TypeId Checker::checkNullableOperand(Expr* e, TypeId expected) {
    const Expr* saved = keepNullable_;
    keepNullable_ = e;
    TypeId t = checkValue(e, expected);
    keepNullable_ = saved;
    return t;
}

} // namespace quartz::sema
