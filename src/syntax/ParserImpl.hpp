#pragma once

#include "syntax/Parser.hpp"

#include <string>

namespace quartz::syntax {

class Parser {
public:
    Parser(const SourceFile& file, Arena& arena, Interner& names, Diagnostics& diags);

    Module* parseModule();

    // declarations (Parser.cpp)
    Decl* parseDecl(std::span<Attribute> attrs, Visibility vis);
    FnDecl* parseFn(std::span<Attribute> attrs, Visibility vis, bool isSuspend);
    Decl* parseStruct(std::span<Attribute> attrs, Visibility vis, DeclKind kind);
    Decl* parseEnum(std::span<Attribute> attrs, Visibility vis);
    Decl* parseImpl(std::span<Attribute> attrs, Visibility vis);
    Decl* parseTrait(std::span<Attribute> attrs, Visibility vis);
    Decl* parseConstDecl(std::span<Attribute> attrs, Visibility vis);
    std::span<Attribute> parseAttributes();
    std::span<TypeParam> parseTypeParams();
    Import parseImport();
    std::span<Symbol> parsePath();

    // statements (Parser.cpp)
    BlockExpr* parseBlock();
    Stmt* parseStmt();
    Stmt* parseLet(bool mutable_, bool isConst, bool isRef);
    Stmt* parseFor();
    void endStmt();

    // expressions and types (ParserExpr.cpp)
    Expr* parseExpr();
    Expr* parseBinary(int minPrec);
    Expr* parseUnary();
    Expr* parsePostfix(Expr* e);
    Expr* parsePrimary();
    Expr* parseInterp();
    Expr* parseParenOrLambda();
    Expr* parseLambda();
    Expr* parseIf();
    Expr* parseMatch();
    Pattern* parsePattern();
    Expr* parseArmBody();
    std::span<Arg> parseArgs();
    Expr* parseWith(Expr* value);
    TypeExpr* parseType();
    std::span<TypeExpr*> parseTypeArgs();
    bool closeAngle();

    // token helpers
    [[nodiscard]] const Token& cur() const { return toks_[pos_]; }
    [[nodiscard]] const Token& peekTok(std::size_t n = 1) const {
        return toks_[pos_ + n < toks_.size() ? pos_ + n : toks_.size() - 1];
    }
    [[nodiscard]] bool at(Tok k) const { return cur().kind == k; }
    const Token& advance() {
        const Token& t = toks_[pos_];
        if (pos_ + 1 < toks_.size()) {
            ++pos_;
        }
        return t;
    }
    bool accept(Tok k) {
        if (at(k)) {
            advance();
            return true;
        }
        return false;
    }
    const Token& expect(Tok k, const char* what);
    Symbol expectIdent(const char* what);
    void error(Span span, std::string message);
    void syncTo(std::initializer_list<Tok> stops);
    bool tooDeep();
    [[nodiscard]] bool newlineSensitive() const { return nlDepth_ == 0; }
    [[nodiscard]] Span prevSpan() const { return toks_[pos_ > 0 ? pos_ - 1 : 0].span; }

    template <class T>
    T* node(Span span) {
        T* n = arena_.make<T>();
        n->span = span;
        return n;
    }

    template <class T>
    std::span<T> list(const std::vector<T>& v) {
        return arena_.copy(v);
    }

    const SourceFile& file_;
    Arena& arena_;
    Interner& names_;
    Diagnostics& diags_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    int nlDepth_ = 0;     // > 0 inside () and [] where newlines are insignificant
    bool noBraceExpr_ = false;
    bool inArm_ = false;
    std::uint32_t lastErrorAt_ = ~0U;
    int depth_ = 0; // tree depth being built; bounded so later recursive passes cannot overflow the stack
    static constexpr int kMaxDepth = 1000;
};

/// Restores the nesting depth on scope exit; nested parse calls and chain links add to it.
class DepthGuard {
public:
    explicit DepthGuard(Parser& p) : p_(p), saved_(p.depth_) { ++p.depth_; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
    ~DepthGuard() { p_.depth_ = saved_; }

private:
    Parser& p_;
    int saved_;
};

/// Saves and restores newline sensitivity: `()` and `[]` ignore newlines, `{}` restores them.
class NewlineGuard {
public:
    NewlineGuard(Parser& p, bool ignore) : p_(p), saved_(p.nlDepth_) { p.nlDepth_ = ignore ? saved_ + 1 : 0; }
    NewlineGuard(const NewlineGuard&) = delete;
    NewlineGuard& operator=(const NewlineGuard&) = delete;
    ~NewlineGuard() { p_.nlDepth_ = saved_; }

private:
    Parser& p_;
    int saved_;
};

} // namespace quartz::syntax
