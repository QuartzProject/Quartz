#include "syntax/ParserImpl.hpp"

#include "syntax/Lexer.hpp"

#include <format>

namespace quartz::syntax {

Parser::Parser(const SourceFile& file, Arena& arena, Interner& names, Diagnostics& diags)
    : file_(file), arena_(arena), names_(names), diags_(diags), toks_(tokenize(file, arena, diags)) {}

void Parser::error(Span span, std::string message) {
    if (span.begin == lastErrorAt_) {
        return;
    }
    lastErrorAt_ = span.begin;
    diags_.error(span, std::move(message));
}

const Token& Parser::expect(Tok k, const char* what) {
    if (at(k)) {
        return advance();
    }
    error(cur().span, std::format("expected {} {}, found {}", tokName(k), what, tokName(cur().kind)));
    return cur();
}

Symbol Parser::expectIdent(const char* what) {
    if (at(Tok::Ident)) {
        return names_.intern(advance().text);
    }
    error(cur().span, std::format("expected {}, found {}", what, tokName(cur().kind)));
    return {};
}

void Parser::syncTo(std::initializer_list<Tok> stops) {
    int depth = 0;
    while (!at(Tok::Eof)) {
        Tok k = cur().kind;
        if (depth == 0) {
            for (Tok s : stops) {
                if (k == s) {
                    return;
                }
            }
        }
        if (k == Tok::LBrace) {
            ++depth;
        } else if (k == Tok::RBrace) {
            if (depth == 0) {
                return;
            }
            --depth;
        }
        advance();
        if (depth == 0 && cur().newlineBefore) {
            return;
        }
    }
}

bool Parser::tooDeep() {
    if (depth_ <= kMaxDepth) {
        return false;
    }
    error(cur().span, "nesting is too deep");
    pos_ = toks_.size() - 1;
    return true;
}

std::span<Symbol> Parser::parsePath() {
    std::vector<Symbol> parts;
    parts.push_back(expectIdent("name"));
    while (at(Tok::Dot) && peekTok().kind == Tok::Ident) {
        advance();
        parts.push_back(names_.intern(advance().text));
    }
    return list(parts);
}

Import Parser::parseImport() {
    Import imp{};
    Span begin = advance().span;
    std::vector<Symbol> parts;
    parts.push_back(expectIdent("package name"));
    while (accept(Tok::Dot)) {
        if (accept(Tok::Star)) {
            imp.star = true;
            break;
        }
        if (at(Tok::LBrace)) {
            advance();
            std::vector<Symbol> names;
            while (!at(Tok::RBrace) && !at(Tok::Eof)) {
                names.push_back(expectIdent("imported name"));
                if (!accept(Tok::Comma)) {
                    break;
                }
            }
            expect(Tok::RBrace, "to close the import list");
            imp.names = list(names);
            break;
        }
        parts.push_back(expectIdent("name"));
    }
    if (accept(Tok::KwAs)) {
        imp.alias = expectIdent("import alias");
    }
    imp.path = list(parts);
    imp.span = begin.to(prevSpan());
    return imp;
}

Module* Parser::parseModule() {
    auto* mod = arena_.make<Module>();
    mod->file = &file_;
    if (accept(Tok::KwPackage)) {
        mod->package = parsePath();
        endStmt();
    }
    std::vector<Import> imports;
    while (at(Tok::KwImport)) {
        imports.push_back(parseImport());
        endStmt();
    }
    mod->imports = list(imports);
    std::vector<Decl*> decls;
    while (!at(Tok::Eof)) {
        std::size_t before = pos_;
        std::span<Attribute> attrs = parseAttributes();
        Visibility vis = Visibility::Private;
        bool privateKeyword = false;
        if (accept(Tok::KwPub)) {
            vis = Visibility::Pub;
        } else if (accept(Tok::KwInternal)) {
            vis = Visibility::Internal;
        } else if (accept(Tok::KwPrivate)) {
            vis = Visibility::Private;
            privateKeyword = true;
        }
        if (at(Tok::KwImport)) {
            error(cur().span, "imports must come before declarations");
            parseImport();
            continue;
        }
        if (Decl* d = parseDecl(attrs, vis)) {
            d->privateKeyword = privateKeyword;
            decls.push_back(d);
        }
        if (pos_ == before) {
            advance();
        }
    }
    mod->decls = list(decls);
    return mod;
}

std::span<Attribute> Parser::parseAttributes() {
    std::vector<Attribute> attrs;
    while (at(Tok::At_)) {
        Span begin = advance().span;
        Attribute a{};
        if (at(Tok::Ident) || (cur().kind >= Tok::KwPackage && cur().kind <= Tok::KwSelf)) {
            a.name = names_.intern(advance().text);
        } else {
            error(cur().span, "expected attribute name");
        }
        if (at(Tok::LParen) && !cur().newlineBefore) {
            advance();
            NewlineGuard g(*this, true);
            std::vector<AttrArg> args;
            while (!at(Tok::RParen) && !at(Tok::Eof)) {
                AttrArg arg{};
                if (at(Tok::Ident) && peekTok().kind == Tok::Eq) {
                    arg.name = names_.intern(advance().text);
                    advance();
                }
                arg.value = parseExpr();
                args.push_back(arg);
                if (!accept(Tok::Comma)) {
                    break;
                }
            }
            expect(Tok::RParen, "to close the attribute arguments");
            a.args = list(args);
        }
        a.span = begin.to(prevSpan());
        attrs.push_back(a);
    }
    return list(attrs);
}

Decl* Parser::parseDecl(std::span<Attribute> attrs, Visibility vis) {
    switch (cur().kind) {
    case Tok::KwFn: return parseFn(attrs, vis, false);
    case Tok::KwSuspend:
        advance();
        if (!at(Tok::KwFn)) {
            error(cur().span, "expected 'fn' after 'suspend'");
            return nullptr;
        }
        return parseFn(attrs, vis, true);
    case Tok::KwStruct: return parseStruct(attrs, vis, DeclKind::Struct);
    case Tok::KwState: return parseStruct(attrs, vis, DeclKind::State);
    case Tok::KwEnum: return parseEnum(attrs, vis);
    case Tok::KwImpl: return parseImpl(attrs, vis);
    case Tok::KwTrait: return parseTrait(attrs, vis);
    case Tok::KwConst: return parseConstDecl(attrs, vis);
    default:
        error(cur().span, std::format("expected a declaration, found {}", tokName(cur().kind)));
        syncTo({Tok::KwFn, Tok::KwStruct, Tok::KwState, Tok::KwEnum, Tok::KwImpl, Tok::KwConst, Tok::At_});
        return nullptr;
    }
}

std::span<TypeParam> Parser::parseTypeParams() {
    std::vector<TypeParam> params;
    if (accept(Tok::Lt)) {
        while (!at(Tok::Gt) && !at(Tok::Eof)) {
            TypeParam p{};
            p.span = cur().span;
            p.name = expectIdent("type parameter");
            if (accept(Tok::Colon)) {
                std::vector<Symbol> bounds;
                do {
                    bounds.push_back(expectIdent("trait name"));
                } while (accept(Tok::Plus));
                p.bounds = list(bounds);
            }
            params.push_back(p);
            if (!accept(Tok::Comma)) {
                break;
            }
        }
        expect(Tok::Gt, "to close the type parameters");
    }
    return list(params);
}

FnDecl* Parser::parseFn(std::span<Attribute> attrs, Visibility vis, bool isSuspend) {
    Span begin = advance().span;
    auto* fn = node<FnDecl>(begin);
    fn->kind = DeclKind::Fn;
    fn->attrs = attrs;
    fn->visibility = vis;
    fn->isSuspend = isSuspend;
    fn->nameSpan = cur().span;
    fn->name = expectIdent("function name");
    fn->typeParams = parseTypeParams();
    expect(Tok::LParen, "to start the parameter list");
    std::vector<Param> params;
    {
        NewlineGuard g(*this, true);
        while (!at(Tok::RParen) && !at(Tok::Eof)) {
            Param p{};
            p.span = cur().span;
            if (params.empty() && at(Tok::KwSelf)) {
                advance();
                fn->hasSelf = true;
            } else {
                p.name = expectIdent("parameter name");
                expect(Tok::Colon, "after the parameter name");
                p.type = parseType();
                if (accept(Tok::Eq)) {
                    p.defaultValue = parseExpr();
                }
                params.push_back(p);
            }
            if (!accept(Tok::Comma)) {
                break;
            }
        }
        expect(Tok::RParen, "to close the parameter list");
    }
    fn->params = list(params);
    if (accept(Tok::Arrow)) {
        fn->result = parseType();
    }
    if (at(Tok::LBrace)) {
        fn->body = parseBlock();
    } else {
        endStmt();
    }
    fn->span = begin.to(prevSpan());
    return fn;
}

Decl* Parser::parseStruct(std::span<Attribute> attrs, Visibility vis, DeclKind kind) {
    Span begin = advance().span;
    auto* s = node<StructDecl>(begin);
    s->kind = kind;
    s->attrs = attrs;
    s->visibility = vis;
    s->nameSpan = cur().span;
    s->name = expectIdent(kind == DeclKind::State ? "state name" : "struct name");
    s->typeParams = parseTypeParams();
    std::vector<FieldDecl> fields;
    if (accept(Tok::LBrace)) {
        NewlineGuard g(*this, false);
        while (!at(Tok::RBrace) && !at(Tok::Eof)) {
            std::size_t before = pos_;
            FieldDecl f{};
            f.attrs = parseAttributes();
            f.span = cur().span;
            f.name = expectIdent("field name");
            expect(Tok::Colon, "after the field name");
            f.type = parseType();
            if (accept(Tok::Eq)) {
                f.defaultValue = parseExpr();
            }
            fields.push_back(f);
            if (!accept(Tok::Comma) && !accept(Tok::Semi) && !at(Tok::RBrace) && !cur().newlineBefore) {
                error(cur().span, "expected a newline or ',' between fields");
                syncTo({Tok::RBrace});
            }
            if (pos_ == before) {
                advance();
            }
        }
        expect(Tok::RBrace, "to close the field list");
    } else {
        endStmt();
    }
    s->fields = list(fields);
    s->span = begin.to(prevSpan());
    return s;
}

Decl* Parser::parseEnum(std::span<Attribute> attrs, Visibility vis) {
    Span begin = advance().span;
    auto* e = node<EnumDecl>(begin);
    e->kind = DeclKind::Enum;
    e->attrs = attrs;
    e->visibility = vis;
    e->nameSpan = cur().span;
    e->name = expectIdent("enum name");
    e->typeParams = parseTypeParams();
    std::vector<Variant> variants;
    expect(Tok::LBrace, "to start the variant list");
    while (!at(Tok::RBrace) && !at(Tok::Eof)) {
        std::size_t before = pos_;
        Variant v{};
        v.span = cur().span;
        v.name = expectIdent("variant name");
        if (accept(Tok::LParen)) {
            NewlineGuard g(*this, true);
            std::vector<TypeExpr*> payload;
            while (!at(Tok::RParen) && !at(Tok::Eof)) {
                payload.push_back(parseType());
                if (!accept(Tok::Comma)) {
                    break;
                }
            }
            expect(Tok::RParen, "to close the variant payload");
            v.payload = list(payload);
        }
        variants.push_back(v);
        if (!accept(Tok::Comma) && !at(Tok::RBrace) && !cur().newlineBefore) {
            error(cur().span, "expected a newline or ',' between variants");
            syncTo({Tok::RBrace});
        }
        if (pos_ == before) {
            advance();
        }
    }
    expect(Tok::RBrace, "to close the variant list");
    e->variants = list(variants);
    e->span = begin.to(prevSpan());
    return e;
}

Decl* Parser::parseImpl(std::span<Attribute> attrs, Visibility vis) {
    Span begin = advance().span;
    auto* impl = node<ImplDecl>(begin);
    impl->kind = DeclKind::Impl;
    impl->attrs = attrs;
    impl->visibility = vis;
    impl->typeParams = parseTypeParams();
    TypeExpr* first = parseType();
    if (accept(Tok::KwFor)) {
        impl->trait = first;
        impl->target = parseType();
    } else {
        impl->target = first;
    }
    std::vector<FnDecl*> methods;
    expect(Tok::LBrace, "to start the impl body");
    while (!at(Tok::RBrace) && !at(Tok::Eof)) {
        std::span<Attribute> fattrs = parseAttributes();
        Visibility fvis = Visibility::Private;
        if (accept(Tok::KwPub)) {
            fvis = Visibility::Pub;
        } else if (accept(Tok::KwInternal)) {
            fvis = Visibility::Internal;
        }
        bool isSuspend = accept(Tok::KwSuspend);
        if (!at(Tok::KwFn)) {
            error(cur().span, std::format("expected 'fn' in impl body, found {}", tokName(cur().kind)));
            syncTo({Tok::KwFn, Tok::RBrace});
            continue;
        }
        methods.push_back(parseFn(fattrs, fvis, isSuspend));
    }
    expect(Tok::RBrace, "to close the impl body");
    impl->methods = list(methods);
    impl->span = begin.to(prevSpan());
    return impl;
}

Decl* Parser::parseTrait(std::span<Attribute> attrs, Visibility vis) {
    Span begin = advance().span;
    auto* t = node<TraitDecl>(begin);
    t->kind = DeclKind::Trait;
    t->attrs = attrs;
    t->visibility = vis;
    t->nameSpan = cur().span;
    t->name = expectIdent("trait name");
    std::vector<FnDecl*> methods;
    expect(Tok::LBrace, "to start the trait body");
    while (!at(Tok::RBrace) && !at(Tok::Eof)) {
        std::span<Attribute> fattrs = parseAttributes();
        bool isSuspend = accept(Tok::KwSuspend);
        if (!at(Tok::KwFn)) {
            error(cur().span, "expected 'fn' in trait body");
            syncTo({Tok::KwFn, Tok::RBrace});
            continue;
        }
        methods.push_back(parseFn(fattrs, Visibility::Pub, isSuspend));
    }
    expect(Tok::RBrace, "to close the trait body");
    t->methods = list(methods);
    t->span = begin.to(prevSpan());
    return t;
}

Decl* Parser::parseConstDecl(std::span<Attribute> attrs, Visibility vis) {
    Span begin = advance().span;
    auto* c = node<ConstDecl>(begin);
    c->kind = DeclKind::Const;
    c->attrs = attrs;
    c->visibility = vis;
    c->nameSpan = cur().span;
    c->name = expectIdent("constant name");
    if (accept(Tok::Colon)) {
        c->type = parseType();
    }
    expect(Tok::Eq, "in constant declaration");
    c->value = parseExpr();
    endStmt();
    c->span = begin.to(prevSpan());
    return c;
}

void Parser::endStmt() {
    if (accept(Tok::Semi) || at(Tok::RBrace) || at(Tok::Eof) || cur().newlineBefore || (inArm_ && at(Tok::Comma))) {
        return;
    }
    error(cur().span, std::format("expected a newline or ';', found {}", tokName(cur().kind)));
    syncTo({Tok::RBrace, Tok::Semi});
    accept(Tok::Semi);
}

BlockExpr* Parser::parseBlock() {
    Span begin = expect(Tok::LBrace, "to start a block").span;
    auto* block = node<BlockExpr>(begin);
    block->kind = ExprKind::Block;
    NewlineGuard g(*this, false);
    bool saved = noBraceExpr_;
    bool savedArm = inArm_;
    noBraceExpr_ = false;
    inArm_ = false;
    std::vector<Stmt*> stmts;
    while (!at(Tok::RBrace) && !at(Tok::Eof)) {
        std::size_t before = pos_;
        if (accept(Tok::Semi)) {
            continue;
        }
        if (Stmt* s = parseStmt()) {
            stmts.push_back(s);
        }
        if (pos_ == before) {
            advance();
        }
    }
    noBraceExpr_ = saved;
    inArm_ = savedArm;
    expect(Tok::RBrace, "to close the block");
    block->stmts = list(stmts);
    block->span = begin.to(prevSpan());
    return block;
}

Stmt* Parser::parseLet(bool mutable_, bool isConst, bool isRef) {
    Span begin = advance().span;
    auto* let = node<LetStmt>(begin);
    let->kind = StmtKind::Let;
    let->mutable_ = mutable_;
    let->isConst = isConst;
    let->isRef = isRef;
    let->nameSpan = cur().span;
    if (!isConst && !isRef && (at(Tok::LParen) || (at(Tok::Ident) && peekTok().kind == Tok::LParen))) {
        let->pattern = parsePattern();
        let->nameSpan = let->pattern->span;
    } else {
        let->name = expectIdent("variable name");
    }
    if (accept(Tok::Colon)) {
        let->type = parseType();
    }
    if (accept(Tok::Eq)) {
        let->init = parseExpr();
    } else if (!mutable_) {
        error(cur().span, "an immutable binding needs an initializer");
    } else if (let->pattern != nullptr) {
        error(cur().span, "a destructuring binding needs an initializer");
    }
    endStmt();
    let->span = begin.to(prevSpan());
    return let;
}

Stmt* Parser::parseFor() {
    Span begin = advance().span;
    auto* f = node<ForStmt>(begin);
    f->kind = StmtKind::For;
    f->nameSpan = cur().span;
    f->name = expectIdent("loop variable");
    if (accept(Tok::Comma)) {
        f->indexName = f->name;
        f->name = expectIdent("loop variable");
    }
    expect(Tok::KwIn, "in for loop");
    noBraceExpr_ = true;
    f->iterable = parseExpr();
    noBraceExpr_ = false;
    f->body = parseBlock();
    f->span = begin.to(prevSpan());
    return f;
}

Stmt* Parser::parseStmt() {
    Span begin = cur().span;
    switch (cur().kind) {
    case Tok::KwLet: return parseLet(false, false, false);
    case Tok::KwVar: return parseLet(true, false, false);
    case Tok::KwConst: return parseLet(false, true, false);
    case Tok::KwRef: return parseLet(false, false, true);
    case Tok::KwFor: return parseFor();
    case Tok::KwWhile: {
        advance();
        auto* w = node<WhileStmt>(begin);
        w->kind = StmtKind::While;
        noBraceExpr_ = true;
        if (accept(Tok::KwLet)) {
            w->bindName = expectIdent("binding name");
            expect(Tok::Eq, "in 'while let'");
        }
        w->cond = parseExpr();
        noBraceExpr_ = false;
        w->body = parseBlock();
        w->span = begin.to(prevSpan());
        return w;
    }
    case Tok::KwReturn: {
        advance();
        auto* r = node<ReturnStmt>(begin);
        r->kind = StmtKind::Return;
        if (!at(Tok::RBrace) && !at(Tok::Semi) && !at(Tok::Eof) && !cur().newlineBefore) {
            r->value = parseExpr();
        }
        endStmt();
        r->span = begin.to(prevSpan());
        return r;
    }
    case Tok::KwBreak:
    case Tok::KwContinue: {
        auto* s = node<Stmt>(begin);
        s->kind = at(Tok::KwBreak) ? StmtKind::Break : StmtKind::Continue;
        advance();
        endStmt();
        return s;
    }
    case Tok::KwScope: {
        advance();
        auto* s = node<ScopeStmt>(begin);
        s->kind = StmtKind::Scope;
        s->body = parseBlock();
        s->span = begin.to(prevSpan());
        return s;
    }
    case Tok::KwEvery: {
        advance();
        auto* s = node<EveryStmt>(begin);
        s->kind = StmtKind::Every;
        noBraceExpr_ = true;
        s->interval = parseExpr();
        noBraceExpr_ = false;
        s->body = parseBlock();
        s->span = begin.to(prevSpan());
        return s;
    }
    case Tok::KwAt: {
        advance();
        auto* s = node<AtStmt>(begin);
        s->kind = StmtKind::At;
        noBraceExpr_ = true;
        s->context = parseExpr();
        noBraceExpr_ = false;
        s->body = parseBlock();
        s->span = begin.to(prevSpan());
        return s;
    }
    default: break;
    }
    Expr* e = parseExpr();
    AssignOp op{};
    bool isAssign = true;
    switch (cur().kind) {
    case Tok::Eq: op = AssignOp::Set; break;
    case Tok::PlusEq: op = AssignOp::Add; break;
    case Tok::MinusEq: op = AssignOp::Sub; break;
    case Tok::StarEq: op = AssignOp::Mul; break;
    case Tok::SlashEq: op = AssignOp::Div; break;
    case Tok::PercentEq: op = AssignOp::Rem; break;
    case Tok::AmpEq: op = AssignOp::BitAnd; break;
    case Tok::PipeEq: op = AssignOp::BitOr; break;
    case Tok::CaretEq: op = AssignOp::BitXor; break;
    case Tok::ShlEq: op = AssignOp::Shl; break;
    case Tok::Gt:
        if (cur().joinedNext && peekTok().kind == Tok::GtEq) {
            advance();
            op = AssignOp::Shr;
        } else {
            isAssign = false;
        }
        break;
    default: isAssign = false; break;
    }
    if (isAssign && !cur().newlineBefore) {
        advance();
        auto* a = node<AssignStmt>(begin);
        a->kind = StmtKind::Assign;
        a->op = op;
        a->target = e;
        a->value = parseExpr();
        endStmt();
        a->span = begin.to(prevSpan());
        return a;
    }
    auto* s = node<ExprStmt>(begin);
    s->kind = StmtKind::Expr;
    s->expr = e;
    bool blockLike = e->kind == ExprKind::If || e->kind == ExprKind::Match || e->kind == ExprKind::Block;
    if (!blockLike) {
        endStmt();
    }
    s->span = begin.to(prevSpan());
    return s;
}

Module* parseModule(const SourceFile& file, Arena& arena, Interner& names, Diagnostics& diags) {
    return Parser(file, arena, names, diags).parseModule();
}

} // namespace quartz::syntax
