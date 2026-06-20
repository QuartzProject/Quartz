#pragma once

#include "support/Interner.hpp"
#include "syntax/Ast.hpp"
#include "syntax/Token.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::format {

using namespace syntax;

/// Binding strength used to decide where an expression needs parentheses (higher binds tighter).
enum Prec : int {
    kPrecLowest = 0,
    kPrecOr = 1,
    kPrecAnd = 2,
    kPrecCompare = 3,
    kPrecCoalesce = 4,
    kPrecRange = 5,
    kPrecBitOr = 6,
    kPrecBitXor = 7,
    kPrecBitAnd = 8,
    kPrecShift = 9,
    kPrecAdd = 10,
    kPrecMul = 11,
    kPrecCast = 12,
    kPrecUnary = 13,
    kPrecPostfix = 14,
    kPrecPrimary = 15,
};

struct Comment {
    std::uint32_t begin;
    std::uint32_t end;
    bool ownLine;      // nothing but whitespace before it on its source line
    bool blankBefore;  // an empty line separates it from what precedes it
    bool newlineAfter; // nothing but whitespace after it on its source line
};

/// Source extent of an expression including the parentheses the user wrapped around it.
struct Extent {
    std::uint32_t begin;
    std::uint32_t end;
    int parens;
};

constexpr std::size_t kNoFence = ~std::size_t{0};

class Printer {
public:
    Printer(std::string_view src, std::span<const Token> toks, std::span<const Span> comments, const Interner& names);

    std::string run(const Module& m);

    // output
    void text(std::string_view s);

    void space() { pendingSpace_ = !lineStart_; }

    void spaceBefore(const Comment& c);
    void breakLine();
    void newline(std::uint32_t next);
    void blankLine();
    void contBreak(std::uint32_t next);
    void flush(std::uint32_t offset);
    void flushTrailing(std::uint32_t before);
    [[nodiscard]] bool commentBefore(std::uint32_t offset) const;

    // source queries
    [[nodiscard]] bool newlineIn(std::uint32_t from, std::uint32_t to) const;
    [[nodiscard]] bool blankLineBefore(std::uint32_t offset) const;
    [[nodiscard]] std::size_t tokenAt(std::uint32_t offset) const;
    [[nodiscard]] std::size_t tokenEndingAt(std::uint32_t end) const;
    [[nodiscard]] std::size_t matching(std::size_t open) const;
    [[nodiscard]] std::size_t nextToken(std::uint32_t from, Tok kind) const;

    [[nodiscard]] std::string_view raw(std::uint32_t begin, std::uint32_t end) const {
        return src_.substr(begin, end - begin);
    }

    [[nodiscard]] std::string_view name(Symbol s) const { return names_.name(s); }

    /// Prints `count` items between brackets, keeping the source's choice of one line or several.
    template <class BeginFn, class EndFn, class ItemFn>
    void items(std::string_view open, std::string_view close, std::uint32_t openEnd, std::uint32_t closeAt,
               std::size_t count, BeginFn begin, EndFn end, ItemFn item, bool singleComma = false);

    // declarations (PrintDecl.cpp)
    void module(const Module& m);
    void imports(const Module& m);
    [[nodiscard]] std::uint32_t declBegin(const Decl& d) const;
    void decl(const Decl& d, bool inTrait);
    void attributes(std::span<const Attribute> attrs);
    void visibility(const Decl& d, bool inTrait);
    void fn(const FnDecl& f);
    void structDecl(const StructDecl& s);
    void enumDecl(const EnumDecl& e);
    void methods(std::span<FnDecl* const> fns, std::uint32_t closeAt, bool inTrait);
    void typeParams(std::span<const TypeParam> params);
    void path(std::span<const Symbol> parts);
    void param(const Param& p);
    [[nodiscard]] std::uint32_t paramEnd(const Param& p) const;
    [[nodiscard]] std::uint32_t nextTokenBegin(std::uint32_t end) const;
    void flushAfterToken(std::uint32_t begin);
    void type(const TypeExpr* t);
    void typeArgs(std::span<TypeExpr* const> args, std::uint32_t closeAt);

    // statements and patterns (PrintStmt.cpp)
    void block(const BlockExpr* b);
    void stmt(const Stmt* s);
    void assignOp(AssignOp op);
    void pattern(const Pattern* p, std::size_t fence = kNoFence);
    void match(const MatchExpr* m);
    void ifExpr(const IfExpr* e);
    void armBody(const Expr* body);

    // expressions (PrintExpr.cpp)
    void expr(const Expr* e, int minPrec = kPrecLowest, std::size_t fence = kNoFence);
    void exprInner(const Expr* e);
    [[nodiscard]] static int precOf(const Expr* e);
    [[nodiscard]] Extent outer(const Expr* e, std::size_t fence = kNoFence) const;
    [[nodiscard]] Extent extent(const Expr* e) const;
    void exprList(std::string_view open, std::string_view close, std::uint32_t openEnd, std::uint32_t closeAt,
                  std::span<Expr* const> elems, bool singleComma);
    void binary(const BinaryExpr* b);
    void call(const CallExpr* c);
    void with(const WithExpr* w);
    void interp(const InterpExpr* e);
    void lambda(const LambdaExpr* l);

private:
    std::string_view src_;
    std::span<const Token> toks_;
    std::vector<Comment> comments_;
    const Interner& names_;
    std::size_t nextComment_ = 0;
    std::string out_;
    int indent_ = 0;
    bool lineStart_ = true;
    bool pendingSpace_ = false;
    bool contActive_ = false;

    friend class Scope;
    friend class Indent;
};

/// Saves the indentation and continuation state for a statement-like unit and restores it on exit.
class Scope {
public:
    explicit Scope(Printer& p) : p_(p), indent_(p.indent_), cont_(p.contActive_) { p.contActive_ = false; }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    ~Scope() {
        p_.indent_ = indent_;
        p_.contActive_ = cont_;
    }

private:
    Printer& p_;
    int indent_;
    bool cont_;
};

/// Raises the indentation by one level for its lifetime, then restores the state it found.
class Indent {
public:
    explicit Indent(Printer& p);
    Indent(const Indent&) = delete;
    Indent& operator=(const Indent&) = delete;
    ~Indent();

private:
    Printer& p_;
    int indent_;
    bool cont_;
};

template <class BeginFn, class EndFn, class ItemFn>
void Printer::items(std::string_view open, std::string_view close, std::uint32_t openEnd, std::uint32_t closeAt,
                    std::size_t count, BeginFn begin, EndFn end, ItemFn item, bool singleComma) {
    flush(openEnd - 1);
    text(open);
    bool multi = count > 0 && (newlineIn(openEnd, begin(0)) || newlineIn(end(count - 1), closeAt));
    for (std::size_t i = 1; i < count && !multi; ++i) {
        multi = newlineIn(end(i - 1), begin(i));
    }
    if (count == 0 && commentBefore(closeAt)) {
        multi = newlineIn(openEnd, closeAt);
    }
    if (!multi) {
        for (std::size_t i = 0; i < count; ++i) {
            if (i > 0) {
                text(",");
                space();
            }
            Scope s(*this);
            item(i);
        }
        if (count == 1 && singleComma) {
            text(",");
        }
        flush(closeAt);
        text(close);
        return;
    }
    {
        Indent in(*this);
        newline(count > 0 ? begin(0) : closeAt);
        for (std::size_t i = 0; i < count; ++i) {
            if (i > 0) {
                if (newlineIn(end(i - 1), begin(i))) {
                    newline(begin(i));
                } else {
                    flushTrailing(begin(i));
                    space();
                }
            }
            {
                Scope s(*this);
                item(i);
            }
            text(",");
        }
        newline(closeAt);
        flush(closeAt);
    }
    newline(closeAt);
    text(close);
}

} // namespace quartz::format
