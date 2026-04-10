#include "format/Format.hpp"
#include "syntax/Ast.hpp"

#include <format>

namespace quartz::format {

using namespace syntax;

namespace {

class Dumper {
public:
    explicit Dumper(const Interner& names) : names_(names) {}

    std::string out;

    void sym(Symbol s) {
        out += ' ';
        out += s.valid() ? names_.name(s) : "~";
    }

    void tag(std::string_view t) {
        out += '(';
        out += t;
    }

    void close() { out += ')'; }

    void path(std::span<const Symbol> p) {
        tag("path");
        for (Symbol s : p) {
            sym(s);
        }
        close();
    }

    void attrs(std::span<const Attribute> as) {
        for (const Attribute& a : as) {
            tag("@");
            sym(a.name);
            for (const AttrArg& arg : a.args) {
                sym(arg.name);
                expr(arg.value);
            }
            close();
        }
    }

    void type(const TypeExpr* t) {
        if (t == nullptr) {
            out += " _";
            return;
        }
        tag(std::format("t{}", static_cast<int>(t->kind)));
        switch (t->kind) {
        case TypeExprKind::Named: {
            const auto* n = static_cast<const NamedTypeExpr*>(t);
            path(n->path);
            for (const TypeExpr* a : n->args) {
                type(a);
            }
            break;
        }
        case TypeExprKind::List:
            type(static_cast<const ListTypeExpr*>(t)->elem);
            break;
        case TypeExprKind::Array:
            type(static_cast<const ArrayTypeExpr*>(t)->elem);
            expr(static_cast<const ArrayTypeExpr*>(t)->size);
            break;
        case TypeExprKind::Tuple:
            for (const TypeExpr* e : static_cast<const TupleTypeExpr*>(t)->elems) {
                type(e);
            }
            break;
        case TypeExprKind::Nullable:
            type(static_cast<const NullableTypeExpr*>(t)->inner);
            break;
        case TypeExprKind::Fn:
            for (const TypeExpr* e : static_cast<const FnTypeExpr*>(t)->params) {
                type(e);
            }
            if (static_cast<const FnTypeExpr*>(t)->result != nullptr) {
                type(static_cast<const FnTypeExpr*>(t)->result);
            }
            break;
        }
        close();
    }

    void params(std::span<const Param> ps) {
        for (const Param& p : ps) {
            tag("param");
            sym(p.name);
            type(p.type);
            expr(p.defaultValue);
            close();
        }
    }

    void exprs(std::span<Expr* const> es) {
        for (const Expr* e : es) {
            expr(e);
        }
    }

    void expr(const Expr* e) {
        if (e == nullptr) {
            out += " _";
            return;
        }
        tag(std::format("e{}", static_cast<int>(e->kind)));
        switch (e->kind) {
        case ExprKind::IntLit:
            out += std::format(" {}", static_cast<const IntLitExpr*>(e)->value);
            break;
        case ExprKind::FloatLit:
            out += std::format(" {}", static_cast<const FloatLitExpr*>(e)->value);
            break;
        case ExprKind::BoolLit:
            out += static_cast<const BoolLitExpr*>(e)->value ? " true" : " false";
            break;
        case ExprKind::CharLit:
            out += std::format(" {}", static_cast<const CharLitExpr*>(e)->value);
            break;
        case ExprKind::StringLit:
            out += std::format(" {:?}", static_cast<const StringLitExpr*>(e)->value);
            break;
        case ExprKind::Interp: {
            const auto* in = static_cast<const InterpExpr*>(e);
            for (std::string_view t : in->texts) {
                out += std::format(" {:?}", t);
            }
            exprs(in->exprs);
            break;
        }
        case ExprKind::Ident: {
            const auto* id = static_cast<const IdentExpr*>(e);
            sym(id->name);
            for (const TypeExpr* t : id->typeArgs) {
                type(t);
            }
            break;
        }
        case ExprKind::Unary: {
            const auto* u = static_cast<const UnaryExpr*>(e);
            out += std::format(" {}", static_cast<int>(u->op));
            expr(u->operand);
            break;
        }
        case ExprKind::Binary: {
            const auto* b = static_cast<const BinaryExpr*>(e);
            out += ' ';
            out += binaryOpText(b->op);
            expr(b->lhs);
            expr(b->rhs);
            break;
        }
        case ExprKind::Call: {
            const auto* c = static_cast<const CallExpr*>(e);
            expr(c->callee);
            for (const Arg& a : c->args) {
                sym(a.name);
                expr(a.value);
            }
            break;
        }
        case ExprKind::Member: {
            const auto* m = static_cast<const MemberExpr*>(e);
            expr(m->object);
            sym(m->name);
            out += m->safe ? " safe" : "";
            break;
        }
        case ExprKind::Index:
            expr(static_cast<const IndexExpr*>(e)->object);
            expr(static_cast<const IndexExpr*>(e)->index);
            break;
        case ExprKind::Propagate:
            expr(static_cast<const PropagateExpr*>(e)->inner);
            break;
        case ExprKind::Range: {
            const auto* r = static_cast<const RangeExpr*>(e);
            out += r->inclusive ? " incl" : "";
            expr(r->lo);
            expr(r->hi);
            break;
        }
        case ExprKind::ListLit:
            exprs(static_cast<const ListLitExpr*>(e)->elems);
            break;
        case ExprKind::ListRepeat:
            expr(static_cast<const ListRepeatExpr*>(e)->value);
            expr(static_cast<const ListRepeatExpr*>(e)->count);
            break;
        case ExprKind::TupleLit:
            exprs(static_cast<const TupleLitExpr*>(e)->elems);
            break;
        case ExprKind::Lambda:
            params(static_cast<const LambdaExpr*>(e)->params);
            expr(static_cast<const LambdaExpr*>(e)->body);
            break;
        case ExprKind::Cast:
            expr(static_cast<const CastExpr*>(e)->value);
            type(static_cast<const CastExpr*>(e)->target);
            break;
        case ExprKind::Is:
            expr(static_cast<const IsExpr*>(e)->value);
            type(static_cast<const IsExpr*>(e)->target);
            break;
        case ExprKind::Wait:
            expr(static_cast<const WaitExpr*>(e)->value);
            break;
        case ExprKind::If: {
            const auto* i = static_cast<const IfExpr*>(e);
            expr(i->cond);
            sym(i->bindName);
            expr(i->bindValue);
            expr(i->then);
            expr(i->otherwise);
            break;
        }
        case ExprKind::Match: {
            const auto* m = static_cast<const MatchExpr*>(e);
            expr(m->subject);
            for (const MatchArm& arm : m->arms) {
                tag("arm");
                pattern(arm.pattern);
                expr(arm.guard);
                expr(arm.body);
                close();
            }
            break;
        }
        case ExprKind::SelfRef:
        case ExprKind::NullLit:
            break;
        case ExprKind::Block:
            for (const Stmt* s : static_cast<const BlockExpr*>(e)->stmts) {
                stmt(s);
            }
            break;
        }
        close();
    }

    void pattern(const Pattern* p) {
        tag(std::format("p{}", static_cast<int>(p->kind)));
        sym(p->name);
        if (p->kind == PatternKind::Variant) {
            path(p->path);
        }
        for (const Pattern* s : p->sub) {
            pattern(s);
        }
        expr(p->literal);
        expr(p->rangeHi);
        close();
    }

    void stmt(const Stmt* s) {
        tag(std::format("s{}", static_cast<int>(s->kind)));
        switch (s->kind) {
        case StmtKind::Expr:
            expr(static_cast<const ExprStmt*>(s)->expr);
            break;
        case StmtKind::Let: {
            const auto* l = static_cast<const LetStmt*>(s);
            out += std::format(" {}{}{}", l->mutable_ ? 'm' : '-', l->isConst ? 'c' : '-', l->isRef ? 'r' : '-');
            sym(l->name);
            type(l->type);
            expr(l->init);
            break;
        }
        case StmtKind::Assign: {
            const auto* a = static_cast<const AssignStmt*>(s);
            out += std::format(" {}", static_cast<int>(a->op));
            expr(a->target);
            expr(a->value);
            break;
        }
        case StmtKind::While:
            expr(static_cast<const WhileStmt*>(s)->cond);
            expr(static_cast<const WhileStmt*>(s)->body);
            break;
        case StmtKind::For: {
            const auto* f = static_cast<const ForStmt*>(s);
            sym(f->indexName);
            sym(f->name);
            expr(f->iterable);
            expr(f->body);
            break;
        }
        case StmtKind::Return:
            expr(static_cast<const ReturnStmt*>(s)->value);
            break;
        case StmtKind::Break:
        case StmtKind::Continue:
            break;
        case StmtKind::Scope:
            expr(static_cast<const ScopeStmt*>(s)->body);
            break;
        case StmtKind::Every:
            expr(static_cast<const EveryStmt*>(s)->interval);
            expr(static_cast<const EveryStmt*>(s)->body);
            break;
        case StmtKind::At:
            expr(static_cast<const AtStmt*>(s)->context);
            expr(static_cast<const AtStmt*>(s)->body);
            break;
        }
        close();
    }

    void typeParams(std::span<const TypeParam> ps) {
        for (const TypeParam& t : ps) {
            tag("tp");
            sym(t.name);
            for (Symbol b : t.bounds) {
                sym(b);
            }
            close();
        }
    }

    void decl(const Decl* d) {
        tag(std::format("d{} v{}", static_cast<int>(d->kind), static_cast<int>(d->visibility)));
        sym(d->name);
        attrs(d->attrs);
        switch (d->kind) {
        case DeclKind::Fn: {
            const auto* f = static_cast<const FnDecl*>(d);
            out += std::format(" {}{}", f->isSuspend ? 's' : '-', f->hasSelf ? 'h' : '-');
            typeParams(f->typeParams);
            params(f->params);
            type(f->result);
            expr(f->body);
            break;
        }
        case DeclKind::Struct:
        case DeclKind::State: {
            const auto* s = static_cast<const StructDecl*>(d);
            typeParams(s->typeParams);
            for (const FieldDecl& f : s->fields) {
                tag("field");
                sym(f.name);
                attrs(f.attrs);
                type(f.type);
                expr(f.defaultValue);
                close();
            }
            break;
        }
        case DeclKind::Enum:
            typeParams(static_cast<const EnumDecl*>(d)->typeParams);
            for (const Variant& v : static_cast<const EnumDecl*>(d)->variants) {
                tag("variant");
                sym(v.name);
                for (const TypeExpr* t : v.payload) {
                    type(t);
                }
                close();
            }
            break;
        case DeclKind::Impl: {
            const auto* i = static_cast<const ImplDecl*>(d);
            typeParams(i->typeParams);
            type(i->target);
            type(i->trait);
            for (const FnDecl* f : i->methods) {
                decl(f);
            }
            break;
        }
        case DeclKind::Trait:
            for (const FnDecl* f : static_cast<const TraitDecl*>(d)->methods) {
                decl(f);
            }
            break;
        case DeclKind::Const: {
            const auto* c = static_cast<const ConstDecl*>(d);
            type(c->type);
            expr(c->value);
            break;
        }
        }
        close();
        out += '\n';
    }

    void module(const Module& m) {
        path(m.package);
        out += '\n';
        for (const Import& imp : m.imports) {
            tag("import");
            path(imp.path);
            for (Symbol s : imp.names) {
                sym(s);
            }
            out += imp.star ? " *" : "";
            sym(imp.alias);
            close();
            out += '\n';
        }
        for (const Decl* d : m.decls) {
            decl(d);
        }
    }

private:
    const Interner& names_;
};

} // namespace

std::string dumpModule(const Module& m, const Interner& names) {
    Dumper d(names);
    d.module(m);
    return std::move(d.out);
}

} // namespace quartz::format
