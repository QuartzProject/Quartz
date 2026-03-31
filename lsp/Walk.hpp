#pragma once

#include "Workspace.hpp"

#include <cstdint>

namespace quartz::lsp {

/// Visits every node of a module; each hook sees a node before its children.
class Walker {
public:
    explicit Walker(const Analysis& a) : a_(a) {}

    Walker(const Walker&) = delete;
    Walker& operator=(const Walker&) = delete;
    virtual ~Walker() = default;

    void module(const syntax::Module& m);

protected:
    virtual void onImport(const syntax::Import&) {}

    virtual void onDecl(const syntax::Decl&) {}

    virtual void onParam(const syntax::Param&) {}

    virtual void onField(const syntax::FieldDecl&, const syntax::StructDecl&) {}

    virtual void onVariant(const syntax::Variant&, const syntax::EnumDecl&) {}

    virtual void onStmt(const syntax::Stmt&) {}

    virtual void onExpr(const syntax::Expr&, const syntax::Expr* /*parent*/) {}

    virtual void onType(const syntax::TypeExpr&) {}

    /// `subject` is the type the pattern matches against, 0 when unknown.
    virtual void onPattern(const syntax::Pattern&, std::uint32_t /*subject*/) {}

    const Analysis& a_;

private:
    void decl(const syntax::Decl* d);
    void fn(const syntax::FnDecl* f);
    void stmt(const syntax::Stmt* s);
    void expr(const syntax::Expr* e, const syntax::Expr* parent);
    void type(const syntax::TypeExpr* t);
    void pattern(const syntax::Pattern* p, std::uint32_t subject);
};

} // namespace quartz::lsp
