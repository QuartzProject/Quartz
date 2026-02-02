#include "Locate.hpp"

namespace quartz::lsp {

using namespace syntax;

namespace {

class Locator {
public:
    Locator(std::uint32_t offset, bool inclusiveEnd) : offset_(offset), inclusive_(inclusiveEnd) {}

    std::vector<Node> path;

    [[nodiscard]] bool contains(Span s) const {
        return s.begin <= offset_ && (offset_ < s.end || (inclusive_ && offset_ == s.end));
    }

    bool leaf(NodeKind kind, Span span, const void* ptr, const void* owner = nullptr) {
        if (ptr == nullptr || !contains(span)) {
            return false;
        }
        path.push_back({kind, span, ptr, owner});
        return true;
    }

    bool decl(const Decl* d) {
        if (d == nullptr || !contains(d->span)) {
            return false;
        }
        path.push_back({NodeKind::Decl, d->span, d});
        if (d->name.valid() && leaf(NodeKind::DeclName, d->nameSpan, d)) {
            return true;
        }
        for (const Attribute& a : d->attrs) {
            for (const AttrArg& arg : a.args) {
                if (expr(arg.value)) {
                    return true;
                }
            }
        }
        switch (d->kind) {
        case DeclKind::Fn:
            return fn(static_cast<const FnDecl*>(d));
        case DeclKind::Struct:
        case DeclKind::State: {
            const auto* s = static_cast<const StructDecl*>(d);
            for (const FieldDecl& f : s->fields) {
                if (leaf(NodeKind::Field, f.span, &f, s) || type(f.type) || expr(f.defaultValue)) {
                    return true;
                }
            }
            return true;
        }
        case DeclKind::Enum: {
            const auto* e = static_cast<const EnumDecl*>(d);
            for (const Variant& v : e->variants) {
                if (leaf(NodeKind::Variant, v.span, &v, e)) {
                    return true;
                }
                for (const TypeExpr* t : v.payload) {
                    if (type(t)) {
                        return true;
                    }
                }
            }
            return true;
        }
        case DeclKind::Impl: {
            const auto* impl = static_cast<const ImplDecl*>(d);
            if (type(impl->trait) || type(impl->target)) {
                return true;
            }
            for (const FnDecl* m : impl->methods) {
                if (decl(m)) {
                    return true;
                }
            }
            return true;
        }
        case DeclKind::Trait:
            for (const FnDecl* m : static_cast<const TraitDecl*>(d)->methods) {
                if (decl(m)) {
                    return true;
                }
            }
            return true;
        case DeclKind::Const: {
            const auto* c = static_cast<const ConstDecl*>(d);
            type(c->type) || expr(c->value);
            return true;
        }
        }
        return true;
    }

    bool fn(const FnDecl* f) {
        for (const Param& p : f->params) {
            if (leaf(NodeKind::Param, p.span, &p, f) || type(p.type) || expr(p.defaultValue)) {
                return true;
            }
        }
        type(f->result) || expr(f->body);
        return true;
    }

    bool stmt(const Stmt* s) {
        if (s == nullptr || !contains(s->span)) {
            return false;
        }
        path.push_back({NodeKind::Stmt, s->span, s});
        switch (s->kind) {
        case StmtKind::Expr:
            expr(static_cast<const ExprStmt*>(s)->expr);
            break;
        case StmtKind::Let: {
            const auto* l = static_cast<const LetStmt*>(s);
            leaf(NodeKind::LetName, l->nameSpan, l) || type(l->type) || expr(l->init);
            break;
        }
        case StmtKind::Assign: {
            const auto* a = static_cast<const AssignStmt*>(s);
            expr(a->target) || expr(a->value);
            break;
        }
        case StmtKind::While: {
            const auto* w = static_cast<const WhileStmt*>(s);
            expr(w->cond) || expr(w->body);
            break;
        }
        case StmtKind::For: {
            const auto* f = static_cast<const ForStmt*>(s);
            leaf(NodeKind::ForName, f->nameSpan, f) || expr(f->iterable) || expr(f->body);
            break;
        }
        case StmtKind::Return:
            expr(static_cast<const ReturnStmt*>(s)->value);
            break;
        case StmtKind::Scope:
            expr(static_cast<const ScopeStmt*>(s)->body);
            break;
        case StmtKind::Every: {
            const auto* e = static_cast<const EveryStmt*>(s);
            expr(e->interval) || expr(e->body);
            break;
        }
        case StmtKind::At: {
            const auto* a = static_cast<const AtStmt*>(s);
            expr(a->context) || expr(a->body);
            break;
        }
        case StmtKind::Break:
        case StmtKind::Continue:
            break;
        }
        return true;
    }

    bool exprs(std::span<Expr* const> list) {
        for (const Expr* e : list) {
            if (expr(e)) {
                return true;
            }
        }
        return false;
    }

    bool types(std::span<TypeExpr* const> list) {
        for (const TypeExpr* t : list) {
            if (type(t)) {
                return true;
            }
        }
        return false;
    }

    bool expr(const Expr* e) {
        if (e == nullptr || !contains(e->span)) {
            return false;
        }
        path.push_back({NodeKind::Expr, e->span, e});
        switch (e->kind) {
        case ExprKind::Interp:
            exprs(static_cast<const InterpExpr*>(e)->exprs);
            break;
        case ExprKind::Ident:
            types(static_cast<const IdentExpr*>(e)->typeArgs);
            break;
        case ExprKind::Unary:
            expr(static_cast<const UnaryExpr*>(e)->operand);
            break;
        case ExprKind::Binary: {
            const auto* b = static_cast<const BinaryExpr*>(e);
            expr(b->lhs) || expr(b->rhs);
            break;
        }
        case ExprKind::Call: {
            const auto* c = static_cast<const CallExpr*>(e);
            if (expr(c->callee)) {
                break;
            }
            for (const Arg& a : c->args) {
                if (expr(a.value)) {
                    break;
                }
            }
            break;
        }
        case ExprKind::Member:
            expr(static_cast<const MemberExpr*>(e)->object);
            break;
        case ExprKind::Index: {
            const auto* i = static_cast<const IndexExpr*>(e);
            expr(i->object) || expr(i->index);
            break;
        }
        case ExprKind::Propagate:
            expr(static_cast<const PropagateExpr*>(e)->inner);
            break;
        case ExprKind::Range: {
            const auto* r = static_cast<const RangeExpr*>(e);
            expr(r->lo) || expr(r->hi);
            break;
        }
        case ExprKind::ListLit:
            exprs(static_cast<const ListLitExpr*>(e)->elems);
            break;
        case ExprKind::ListRepeat: {
            const auto* l = static_cast<const ListRepeatExpr*>(e);
            expr(l->value) || expr(l->count);
            break;
        }
        case ExprKind::TupleLit:
            exprs(static_cast<const TupleLitExpr*>(e)->elems);
            break;
        case ExprKind::Lambda: {
            const auto* l = static_cast<const LambdaExpr*>(e);
            for (const Param& p : l->params) {
                if (leaf(NodeKind::Param, p.span, &p, l) || type(p.type)) {
                    return true;
                }
            }
            expr(l->body);
            break;
        }
        case ExprKind::Cast: {
            const auto* c = static_cast<const CastExpr*>(e);
            expr(c->value) || type(c->target);
            break;
        }
        case ExprKind::Is: {
            const auto* c = static_cast<const IsExpr*>(e);
            expr(c->value) || type(c->target);
            break;
        }
        case ExprKind::Wait:
            expr(static_cast<const WaitExpr*>(e)->value);
            break;
        case ExprKind::If: {
            const auto* i = static_cast<const IfExpr*>(e);
            expr(i->cond) || expr(i->bindValue) || expr(i->then) || expr(i->otherwise);
            break;
        }
        case ExprKind::Match: {
            const auto* m = static_cast<const MatchExpr*>(e);
            if (expr(m->subject)) {
                break;
            }
            for (const MatchArm& arm : m->arms) {
                Span s = arm.pattern != nullptr ? arm.pattern->span : Span{};
                if (arm.body != nullptr) {
                    s.end = arm.body->span.end;
                }
                if (leaf(NodeKind::MatchArm, s, &arm, m)) {
                    pattern(arm.pattern) || expr(arm.guard) || expr(arm.body);
                    break;
                }
            }
            break;
        }
        case ExprKind::Block:
            for (const Stmt* s : static_cast<const BlockExpr*>(e)->stmts) {
                if (stmt(s)) {
                    break;
                }
            }
            break;
        default:
            break;
        }
        return true;
    }

    bool pattern(const Pattern* p) {
        if (p == nullptr || !contains(p->span)) {
            return false;
        }
        path.push_back({NodeKind::Pattern, p->span, p});
        for (const Pattern* s : p->sub) {
            if (pattern(s)) {
                return true;
            }
        }
        expr(p->literal) || expr(p->rangeHi);
        return true;
    }

    bool type(const TypeExpr* t) {
        if (t == nullptr || !contains(t->span)) {
            return false;
        }
        path.push_back({NodeKind::Type, t->span, t});
        switch (t->kind) {
        case TypeExprKind::Named:
            types(static_cast<const NamedTypeExpr*>(t)->args);
            break;
        case TypeExprKind::List:
            type(static_cast<const ListTypeExpr*>(t)->elem);
            break;
        case TypeExprKind::Array: {
            const auto* a = static_cast<const ArrayTypeExpr*>(t);
            type(a->elem) || expr(a->size);
            break;
        }
        case TypeExprKind::Tuple:
            types(static_cast<const TupleTypeExpr*>(t)->elems);
            break;
        case TypeExprKind::Nullable:
            type(static_cast<const NullableTypeExpr*>(t)->inner);
            break;
        case TypeExprKind::Fn: {
            const auto* f = static_cast<const FnTypeExpr*>(t);
            types(f->params) || type(f->result);
            break;
        }
        }
        return true;
    }

private:
    std::uint32_t offset_;
    bool inclusive_;
};

} // namespace

std::vector<Node> locate(const Module& m, std::uint32_t offset, bool inclusiveEnd) {
    Locator l(offset, inclusiveEnd);
    for (const Decl* d : m.decls) {
        if (l.decl(d)) {
            break;
        }
    }
    return std::move(l.path);
}

} // namespace quartz::lsp
