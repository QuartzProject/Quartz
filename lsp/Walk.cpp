#include "Walk.hpp"

namespace quartz::lsp {

using namespace syntax;

void Walker::module(const Module& m) {
    for (const Import& imp : m.imports) {
        onImport(imp);
    }
    for (const Decl* d : m.decls) {
        decl(d);
    }
}

void Walker::decl(const Decl* d) {
    if (d == nullptr) {
        return;
    }
    onDecl(*d);
    for (const Attribute& a : d->attrs) {
        for (const AttrArg& arg : a.args) {
            expr(arg.value, nullptr);
        }
    }
    switch (d->kind) {
    case DeclKind::Fn:
        fn(static_cast<const FnDecl*>(d));
        break;
    case DeclKind::Struct:
    case DeclKind::State: {
        const auto* s = static_cast<const StructDecl*>(d);
        for (const FieldDecl& f : s->fields) {
            onField(f, *s);
            type(f.type);
            expr(f.defaultValue, nullptr);
        }
        break;
    }
    case DeclKind::Enum: {
        const auto* e = static_cast<const EnumDecl*>(d);
        for (const Variant& v : e->variants) {
            onVariant(v, *e);
            for (const TypeExpr* t : v.payload) {
                type(t);
            }
        }
        break;
    }
    case DeclKind::Impl: {
        const auto* impl = static_cast<const ImplDecl*>(d);
        type(impl->trait);
        type(impl->target);
        for (const FnDecl* m : impl->methods) {
            decl(m);
        }
        break;
    }
    case DeclKind::Trait:
        for (const FnDecl* m : static_cast<const TraitDecl*>(d)->methods) {
            decl(m);
        }
        break;
    case DeclKind::Const: {
        const auto* c = static_cast<const ConstDecl*>(d);
        type(c->type);
        expr(c->value, nullptr);
        break;
    }
    }
}

void Walker::fn(const FnDecl* f) {
    for (const Param& p : f->params) {
        onParam(p);
        type(p.type);
        expr(p.defaultValue, nullptr);
    }
    type(f->result);
    expr(f->body, nullptr);
}

void Walker::stmt(const Stmt* s) {
    if (s == nullptr) {
        return;
    }
    onStmt(*s);
    switch (s->kind) {
    case StmtKind::Expr:
        expr(static_cast<const ExprStmt*>(s)->expr, nullptr);
        break;
    case StmtKind::Let: {
        const auto* l = static_cast<const LetStmt*>(s);
        type(l->type);
        expr(l->init, nullptr);
        pattern(l->pattern, l->type != nullptr ? l->type->type : l->init != nullptr ? l->init->type : 0);
        break;
    }
    case StmtKind::Assign: {
        const auto* a = static_cast<const AssignStmt*>(s);
        expr(a->target, nullptr);
        expr(a->value, nullptr);
        break;
    }
    case StmtKind::While: {
        const auto* w = static_cast<const WhileStmt*>(s);
        expr(w->cond, nullptr);
        expr(w->body, nullptr);
        break;
    }
    case StmtKind::For: {
        const auto* f = static_cast<const ForStmt*>(s);
        expr(f->iterable, nullptr);
        expr(f->body, nullptr);
        break;
    }
    case StmtKind::Return:
        expr(static_cast<const ReturnStmt*>(s)->value, nullptr);
        break;
    case StmtKind::Scope:
        expr(static_cast<const ScopeStmt*>(s)->body, nullptr);
        break;
    case StmtKind::Every: {
        const auto* e = static_cast<const EveryStmt*>(s);
        expr(e->interval, nullptr);
        expr(e->body, nullptr);
        break;
    }
    case StmtKind::At: {
        const auto* a = static_cast<const AtStmt*>(s);
        expr(a->context, nullptr);
        expr(a->body, nullptr);
        break;
    }
    case StmtKind::Break:
    case StmtKind::Continue:
        break;
    }
}

void Walker::expr(const Expr* e, const Expr* parent) {
    if (e == nullptr) {
        return;
    }
    onExpr(*e, parent);
    switch (e->kind) {
    case ExprKind::Interp:
        for (const Expr* x : static_cast<const InterpExpr*>(e)->exprs) {
            expr(x, e);
        }
        break;
    case ExprKind::Ident:
        for (const TypeExpr* t : static_cast<const IdentExpr*>(e)->typeArgs) {
            type(t);
        }
        break;
    case ExprKind::Unary:
        expr(static_cast<const UnaryExpr*>(e)->operand, e);
        break;
    case ExprKind::Binary: {
        const auto* b = static_cast<const BinaryExpr*>(e);
        expr(b->lhs, e);
        expr(b->rhs, e);
        break;
    }
    case ExprKind::Call: {
        const auto* c = static_cast<const CallExpr*>(e);
        expr(c->callee, e);
        for (const Arg& a : c->args) {
            expr(a.value, e);
        }
        break;
    }
    case ExprKind::Member:
        expr(static_cast<const MemberExpr*>(e)->object, e);
        break;
    case ExprKind::Index: {
        const auto* i = static_cast<const IndexExpr*>(e);
        expr(i->object, e);
        expr(i->index, e);
        break;
    }
    case ExprKind::Propagate:
        expr(static_cast<const PropagateExpr*>(e)->inner, e);
        break;
    case ExprKind::Range: {
        const auto* r = static_cast<const RangeExpr*>(e);
        expr(r->lo, e);
        expr(r->hi, e);
        break;
    }
    case ExprKind::ListLit:
        for (const Expr* x : static_cast<const ListLitExpr*>(e)->elems) {
            expr(x, e);
        }
        break;
    case ExprKind::ListRepeat: {
        const auto* l = static_cast<const ListRepeatExpr*>(e);
        expr(l->value, e);
        expr(l->count, e);
        break;
    }
    case ExprKind::TupleLit:
        for (const Expr* x : static_cast<const TupleLitExpr*>(e)->elems) {
            expr(x, e);
        }
        break;
    case ExprKind::Lambda: {
        const auto* l = static_cast<const LambdaExpr*>(e);
        for (const Param& p : l->params) {
            onParam(p);
            type(p.type);
        }
        expr(l->body, e);
        break;
    }
    case ExprKind::Cast: {
        const auto* c = static_cast<const CastExpr*>(e);
        expr(c->value, e);
        type(c->target);
        break;
    }
    case ExprKind::Is: {
        const auto* c = static_cast<const IsExpr*>(e);
        expr(c->value, e);
        pattern(c->pattern, c->value->type);
        break;
    }
    case ExprKind::With:
        expr(static_cast<const WithExpr*>(e)->value, e);
        for (const Arg& a : static_cast<const WithExpr*>(e)->fields) {
            expr(a.value, e);
        }
        break;
    case ExprKind::Wait:
        expr(static_cast<const WaitExpr*>(e)->value, e);
        break;
    case ExprKind::If: {
        const auto* i = static_cast<const IfExpr*>(e);
        expr(i->cond, e);
        expr(i->bindValue, e);
        expr(i->then, e);
        expr(i->otherwise, e);
        break;
    }
    case ExprKind::Match: {
        const auto* m = static_cast<const MatchExpr*>(e);
        expr(m->subject, e);
        std::uint32_t subject = m->subject != nullptr ? m->subject->type : 0;
        for (const MatchArm& arm : m->arms) {
            pattern(arm.pattern, subject);
            expr(arm.guard, e);
            expr(arm.body, e);
        }
        break;
    }
    case ExprKind::Block:
        for (const Stmt* s : static_cast<const BlockExpr*>(e)->stmts) {
            stmt(s);
        }
        break;
    default:
        break;
    }
}

void Walker::type(const TypeExpr* t) {
    if (t == nullptr) {
        return;
    }
    onType(*t);
    switch (t->kind) {
    case TypeExprKind::Named:
        for (const TypeExpr* x : static_cast<const NamedTypeExpr*>(t)->args) {
            type(x);
        }
        break;
    case TypeExprKind::List:
        type(static_cast<const ListTypeExpr*>(t)->elem);
        break;
    case TypeExprKind::Array: {
        const auto* a = static_cast<const ArrayTypeExpr*>(t);
        type(a->elem);
        expr(a->size, nullptr);
        break;
    }
    case TypeExprKind::Tuple:
        for (const TypeExpr* x : static_cast<const TupleTypeExpr*>(t)->elems) {
            type(x);
        }
        break;
    case TypeExprKind::Nullable:
        type(static_cast<const NullableTypeExpr*>(t)->inner);
        break;
    case TypeExprKind::Fn: {
        const auto* f = static_cast<const FnTypeExpr*>(t);
        for (const TypeExpr* x : f->params) {
            type(x);
        }
        type(f->result);
        break;
    }
    }
}

void Walker::pattern(const Pattern* p, std::uint32_t subject) {
    if (p == nullptr) {
        return;
    }
    const sema::Program* prog = a_.program();
    if (prog != nullptr && subject != 0 && subject < prog->types.size() &&
        prog->types.kind(subject) == ir::TypeKind::Nullable && p->kind != PatternKind::Binding) {
        subject = prog->types.info(subject).elem; // patterns other than names see through `null`
    }
    onPattern(*p, subject);
    const std::vector<std::uint32_t>* payload = nullptr;
    std::vector<std::uint32_t> fields;
    if (prog != nullptr && subject != 0 && subject < prog->types.size()) {
        const ir::TypeInfo& info = prog->types.info(subject);
        if (p->kind == PatternKind::Variant && info.kind == ir::TypeKind::Enum &&
            p->variantIndex < info.variants.size()) {
            payload = &info.variants[p->variantIndex].payload;
        } else if (p->kind == PatternKind::Tuple && info.kind == ir::TypeKind::Tuple) {
            payload = &info.elems;
        } else if (p->kind == PatternKind::Struct && info.kind == ir::TypeKind::Struct) {
            for (const ir::Field& f : info.fields) {
                fields.push_back(f.type);
            }
            payload = &fields;
        }
    }
    for (std::size_t i = 0; i < p->sub.size(); ++i) {
        pattern(p->sub[i], payload != nullptr && i < payload->size() ? (*payload)[i] : 0);
    }
    expr(p->literal, nullptr);
    expr(p->rangeHi, nullptr);
}

} // namespace quartz::lsp
