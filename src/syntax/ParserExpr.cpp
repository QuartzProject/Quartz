#include "syntax/ParserImpl.hpp"

#include <format>

namespace quartz::syntax {

const char* binaryOpText(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Rem: return "%";
    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitOr: return "|";
    case BinaryOp::BitXor: return "^";
    case BinaryOp::Shl: return "<<";
    case BinaryOp::Shr: return ">>";
    case BinaryOp::Eq: return "==";
    case BinaryOp::Ne: return "!=";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Ge: return ">=";
    case BinaryOp::And: return "&&";
    case BinaryOp::Or: return "||";
    case BinaryOp::Coalesce: return "??";
    }
    return "?";
}

namespace {

constexpr int kPrecAs = 12;

struct OpInfo {
    BinaryOp op;
    int prec;
    bool rightAssoc;
    bool nonAssoc;
    int tokens; // 2 for `>>`
};

bool binaryOp(const Parser& p, OpInfo& out) {
    const Token& t = p.cur();
    switch (t.kind) {
    case Tok::PipePipe: out = {BinaryOp::Or, 1, false, false, 1}; return true;
    case Tok::AmpAmp: out = {BinaryOp::And, 2, false, false, 1}; return true;
    case Tok::EqEq: out = {BinaryOp::Eq, 3, false, true, 1}; return true;
    case Tok::BangEq: out = {BinaryOp::Ne, 3, false, true, 1}; return true;
    case Tok::Lt: out = {BinaryOp::Lt, 3, false, true, 1}; return true;
    case Tok::LtEq: out = {BinaryOp::Le, 3, false, true, 1}; return true;
    case Tok::GtEq: out = {BinaryOp::Ge, 3, false, true, 1}; return true;
    case Tok::Gt:
        if (t.joinedNext && p.peekTok().kind == Tok::Gt) {
            out = {BinaryOp::Shr, 9, false, false, 2};
        } else if (t.joinedNext && p.peekTok().kind == Tok::GtEq) {
            return false; // `>>=` is an assignment
        } else {
            out = {BinaryOp::Gt, 3, false, true, 1};
        }
        return true;
    case Tok::QuestionQuestion: out = {BinaryOp::Coalesce, 4, true, false, 1}; return true;
    case Tok::Pipe: out = {BinaryOp::BitOr, 6, false, false, 1}; return true;
    case Tok::Caret: out = {BinaryOp::BitXor, 7, false, false, 1}; return true;
    case Tok::Amp: out = {BinaryOp::BitAnd, 8, false, false, 1}; return true;
    case Tok::Shl: out = {BinaryOp::Shl, 9, false, false, 1}; return true;
    case Tok::Plus: out = {BinaryOp::Add, 10, false, false, 1}; return true;
    case Tok::Minus: out = {BinaryOp::Sub, 10, false, false, 1}; return true;
    case Tok::Star: out = {BinaryOp::Mul, 11, false, false, 1}; return true;
    case Tok::Slash: out = {BinaryOp::Div, 11, false, false, 1}; return true;
    case Tok::Percent: out = {BinaryOp::Rem, 11, false, false, 1}; return true;
    default: return false;
    }
}

bool continuesAcrossNewline(BinaryOp op) {
    return op == BinaryOp::And || op == BinaryOp::Or || op == BinaryOp::Coalesce;
}

} // namespace

Expr* Parser::parseExpr() { return parseBinary(1); }

Expr* Parser::parseBinary(int minPrec) {
    DepthGuard guard(*this);
    Expr* lhs = parseUnary();
    int lastNonAssoc = -1;
    while (true) {
        ++depth_;
        if (tooDeep()) {
            break;
        }
        if ((at(Tok::DotDot) || at(Tok::DotDotEq)) && minPrec <= 5 &&
            !(cur().newlineBefore && newlineSensitive())) {
            bool inclusive = advance().kind == Tok::DotDotEq;
            auto* r = node<RangeExpr>(lhs->span);
            r->kind = ExprKind::Range;
            r->lo = lhs;
            r->hi = parseBinary(6);
            r->inclusive = inclusive;
            r->span = lhs->span.to(prevSpan());
            lhs = r;
            continue;
        }
        if ((at(Tok::KwAs) || at(Tok::KwIs)) && minPrec <= kPrecAs && !(cur().newlineBefore && newlineSensitive())) {
            if (advance().kind == Tok::KwAs) {
                TypeExpr* target = parseType();
                auto* c = node<CastExpr>(lhs->span.to(prevSpan()));
                c->kind = ExprKind::Cast;
                c->value = lhs;
                c->target = target;
                lhs = c;
            } else {
                Pattern* pattern = parsePattern();
                auto* c = node<IsExpr>(lhs->span.to(prevSpan()));
                c->kind = ExprKind::Is;
                c->value = lhs;
                c->pattern = pattern;
                lhs = c;
            }
            continue;
        }
        if (at(Tok::Ident) && cur().text == "with" && peekTok().kind == Tok::LBrace && minPrec <= kPrecAs &&
            !(cur().newlineBefore && newlineSensitive())) {
            advance();
            lhs = parseWith(lhs);
            continue;
        }
        OpInfo info{};
        if (!binaryOp(*this, info) || info.prec < minPrec) {
            break;
        }
        if (cur().newlineBefore && newlineSensitive() && !continuesAcrossNewline(info.op)) {
            break;
        }
        if (info.nonAssoc && info.prec == lastNonAssoc) {
            error(cur().span, "comparison operators cannot be chained; use parentheses");
        }
        for (int i = 0; i < info.tokens; ++i) {
            advance();
        }
        Expr* rhs = parseBinary(info.rightAssoc ? info.prec : info.prec + 1);
        auto* b = node<BinaryExpr>(lhs->span.to(rhs->span));
        b->kind = ExprKind::Binary;
        b->op = info.op;
        b->lhs = lhs;
        b->rhs = rhs;
        lhs = b;
        lastNonAssoc = info.nonAssoc ? info.prec : -1;
    }
    return lhs;
}

Expr* Parser::parseUnary() {
    DepthGuard guard(*this);
    Span begin = cur().span;
    if (tooDeep()) {
        auto* e = node<NullLitExpr>(begin);
        e->kind = ExprKind::NullLit;
        return e;
    }
    UnaryOp op{};
    switch (cur().kind) {
    case Tok::Minus: op = UnaryOp::Neg; break;
    case Tok::Bang: op = UnaryOp::Not; break;
    case Tok::Tilde: op = UnaryOp::BitNot; break;
    case Tok::KwWait: {
        advance();
        auto* w = node<WaitExpr>(begin);
        w->kind = ExprKind::Wait;
        w->value = parseBinary(5);
        w->span = begin.to(prevSpan());
        return w;
    }
    default: return parsePostfix(parsePrimary());
    }
    advance();
    Expr* operand = parseUnary();
    auto* u = node<UnaryExpr>(begin.to(operand->span));
    u->kind = ExprKind::Unary;
    u->op = op;
    u->operand = operand;
    return u;
}

std::span<Arg> Parser::parseArgs() {
    NewlineGuard g(*this, true);
    std::vector<Arg> args;
    while (!at(Tok::RParen) && !at(Tok::Eof)) {
        Arg a{};
        if (at(Tok::Ident) && peekTok().kind == Tok::Eq) {
            a.name = names_.intern(advance().text);
            advance();
        }
        a.value = parseExpr();
        args.push_back(a);
        if (!accept(Tok::Comma)) {
            break;
        }
    }
    expect(Tok::RParen, "to close the argument list");
    return list(args);
}

// `with` is contextual: an identifier everywhere else, so older code that names something `with` still parses.
Expr* Parser::parseWith(Expr* value) {
    auto* w = node<WithExpr>(value->span);
    w->kind = ExprKind::With;
    w->value = value;
    expect(Tok::LBrace, "after 'with'");
    NewlineGuard g(*this, false);
    std::vector<Arg> fields;
    while (!at(Tok::RBrace) && !at(Tok::Eof)) {
        Arg a{};
        a.name = expectIdent("field name");
        if (!a.name.valid()) {
            syncTo({Tok::RBrace});
            break;
        }
        expect(Tok::Eq, "after the field name");
        a.value = parseExpr();
        fields.push_back(a);
        if (!accept(Tok::Comma) && !at(Tok::RBrace) && !cur().newlineBefore) {
            error(cur().span, std::format("expected ',' or '}}' after a field, found {}", tokName(cur().kind)));
            syncTo({Tok::RBrace});
        }
    }
    expect(Tok::RBrace, "to close 'with'");
    w->fields = list(fields);
    w->span = value->span.to(prevSpan());
    return w;
}

Expr* Parser::parsePostfix(Expr* e) {
    DepthGuard guard(*this);
    while (true) {
        ++depth_;
        if (tooDeep()) {
            return e;
        }
        bool nl = cur().newlineBefore && newlineSensitive();
        if (at(Tok::Dot) || at(Tok::QuestionDot)) {
            bool safe = advance().kind == Tok::QuestionDot;
            auto* m = node<MemberExpr>(e->span);
            m->kind = ExprKind::Member;
            m->object = e;
            m->safe = safe;
            if (at(Tok::Ident)) {
                m->name = names_.intern(advance().text);
            } else if (at(Tok::Int)) {
                m->name = names_.intern(advance().text); // tuple field `.0`
            } else {
                error(cur().span, "expected a member name after '.'");
            }
            m->span = e->span.to(prevSpan());
            e = m;
        } else if (at(Tok::LParen) && !nl) {
            advance();
            auto* c = node<CallExpr>(e->span);
            c->kind = ExprKind::Call;
            c->callee = e;
            c->args = parseArgs();
            c->span = e->span.to(prevSpan());
            e = c;
        } else if (at(Tok::LBracket) && !nl) {
            advance();
            NewlineGuard g(*this, true);
            auto* ix = node<IndexExpr>(e->span);
            ix->kind = ExprKind::Index;
            ix->object = e;
            ix->index = parseExpr();
            expect(Tok::RBracket, "to close the index");
            ix->span = e->span.to(prevSpan());
            e = ix;
        } else if (at(Tok::Question) && !nl) {
            advance();
            auto* p = node<PropagateExpr>(e->span.to(prevSpan()));
            p->kind = ExprKind::Propagate;
            p->inner = e;
            e = p;
        } else {
            return e;
        }
    }
}

Expr* Parser::parseInterp() {
    Span begin = cur().span;
    std::vector<std::string_view> texts;
    std::vector<Expr*> exprs;
    std::vector<std::string_view> specs;
    texts.push_back(advance().text);
    while (true) {
        {
            NewlineGuard g(*this, true);
            exprs.push_back(parseExpr());
        }
        specs.push_back(at(Tok::FormatSpec) ? advance().text : std::string_view{});
        if (at(Tok::InterpMid)) {
            texts.push_back(advance().text);
            continue;
        }
        if (at(Tok::InterpEnd)) {
            texts.push_back(advance().text);
            break;
        }
        error(cur().span, "expected '}' to close the interpolation");
        texts.emplace_back();
        break;
    }
    auto* in = node<InterpExpr>(begin.to(prevSpan()));
    in->kind = ExprKind::Interp;
    in->texts = list(texts);
    in->exprs = list(exprs);
    in->specs = list(specs);
    return in;
}

Expr* Parser::parseLambda() {
    Span begin = expect(Tok::LParen, "to start lambda parameters").span;
    std::vector<Param> params;
    {
        NewlineGuard g(*this, true);
        while (!at(Tok::RParen) && !at(Tok::Eof)) {
            Param p{};
            p.span = cur().span;
            p.name = expectIdent("lambda parameter");
            if (accept(Tok::Colon)) {
                p.type = parseType();
            }
            params.push_back(p);
            if (!accept(Tok::Comma)) {
                break;
            }
        }
        expect(Tok::RParen, "to close lambda parameters");
    }
    expect(Tok::FatArrow, "in lambda");
    auto* l = node<LambdaExpr>(begin);
    l->kind = ExprKind::Lambda;
    l->params = list(params);
    l->body = at(Tok::LBrace) ? parseBlock() : parseExpr();
    l->span = begin.to(prevSpan());
    return l;
}

Expr* Parser::parseParenOrLambda() {
    // Bounded lookahead (spec 3.6): skip to the matching `)` and look for `=>`.
    std::size_t i = pos_;
    int depth = 0;
    for (; i < toks_.size(); ++i) {
        Tok k = toks_[i].kind;
        if (k == Tok::LParen || k == Tok::LBracket || k == Tok::LBrace) {
            ++depth;
        } else if (k == Tok::RParen || k == Tok::RBracket || k == Tok::RBrace) {
            if (--depth == 0) {
                break;
            }
        } else if (k == Tok::Eof) {
            break;
        }
    }
    if (i + 1 < toks_.size() && toks_[i].kind == Tok::RParen && toks_[i + 1].kind == Tok::FatArrow) {
        return parseLambda();
    }
    Span begin = advance().span;
    NewlineGuard g(*this, true);
    if (accept(Tok::RParen)) {
        auto* t = node<TupleLitExpr>(begin.to(prevSpan()));
        t->kind = ExprKind::TupleLit;
        return t;
    }
    Expr* first = parseExpr();
    if (!at(Tok::Comma)) {
        expect(Tok::RParen, "to close the parenthesis");
        return first;
    }
    std::vector<Expr*> elems{first};
    while (accept(Tok::Comma) && !at(Tok::RParen)) {
        elems.push_back(parseExpr());
    }
    expect(Tok::RParen, "to close the tuple");
    auto* t = node<TupleLitExpr>(begin.to(prevSpan()));
    t->kind = ExprKind::TupleLit;
    t->elems = list(elems);
    return t;
}

Expr* Parser::parseIf() {
    Span begin = advance().span;
    auto* e = node<IfExpr>(begin);
    e->kind = ExprKind::If;
    bool saved = noBraceExpr_;
    noBraceExpr_ = true;
    if (accept(Tok::KwLet)) {
        e->bindName = expectIdent("binding name");
        expect(Tok::Eq, "in 'if let'");
        e->bindValue = parseExpr();
    } else {
        e->cond = parseExpr();
    }
    noBraceExpr_ = saved;
    e->then = parseBlock();
    if (at(Tok::KwElse)) {
        advance();
        e->otherwise = at(Tok::KwIf) ? parseIf() : parseBlock();
    }
    e->span = begin.to(prevSpan());
    return e;
}

Pattern* Parser::parsePattern() {
    DepthGuard guard(*this);
    Span begin = cur().span;
    auto* p = arena_.make<Pattern>();
    p->span = begin;
    if (tooDeep()) {
        p->kind = PatternKind::Wildcard;
        return p;
    }
    if (at(Tok::Ident) && cur().text == "_") {
        advance();
        p->kind = PatternKind::Wildcard;
        return p;
    }
    if (accept(Tok::LParen)) {
        NewlineGuard g(*this, true);
        std::vector<Pattern*> sub;
        bool comma = false;
        while (!at(Tok::RParen) && !at(Tok::Eof)) {
            sub.push_back(parsePattern());
            if (!accept(Tok::Comma)) {
                break;
            }
            comma = true;
        }
        expect(Tok::RParen, "to close the tuple pattern");
        if (sub.size() == 1 && !comma) {
            return sub[0]; // `(p)` only groups
        }
        p->kind = PatternKind::Tuple;
        p->sub = list(sub);
        p->span = begin.to(prevSpan());
        return p;
    }
    if (at(Tok::Ident)) {
        std::span<Symbol> path = parsePath();
        if (path.size() == 1 && !at(Tok::LParen)) {
            p->kind = PatternKind::Binding;
            p->name = path[0];
            p->path = path;
            return p;
        }
        p->kind = PatternKind::Variant;
        p->path = path;
        p->name = path[path.size() - 1];
        if (accept(Tok::LParen)) {
            NewlineGuard g(*this, true);
            std::vector<Pattern*> sub;
            while (!at(Tok::RParen) && !at(Tok::Eof)) {
                sub.push_back(parsePattern());
                if (!accept(Tok::Comma)) {
                    break;
                }
            }
            expect(Tok::RParen, "to close the variant pattern");
            p->sub = list(sub);
        }
        p->span = begin.to(prevSpan());
        return p;
    }
    p->kind = PatternKind::Literal;
    p->literal = parseUnary();
    if (at(Tok::DotDot) || at(Tok::DotDotEq)) {
        p->inclusive = advance().kind == Tok::DotDotEq;
        p->kind = PatternKind::Range;
        p->rangeHi = parseUnary();
    }
    p->span = begin.to(prevSpan());
    return p;
}

Expr* Parser::parseArmBody() {
    if (at(Tok::LBrace)) {
        return parseBlock();
    }
    bool saved = inArm_;
    inArm_ = true;
    Span begin = cur().span;
    Stmt* s = parseStmt();
    inArm_ = saved;
    if (s->kind == StmtKind::Expr) {
        return static_cast<ExprStmt*>(s)->expr;
    }
    auto* block = node<BlockExpr>(begin.to(prevSpan()));
    block->kind = ExprKind::Block;
    std::vector<Stmt*> one{s};
    block->stmts = list(one);
    return block;
}

Expr* Parser::parseMatch() {
    Span begin = advance().span;
    auto* m = node<MatchExpr>(begin);
    m->kind = ExprKind::Match;
    noBraceExpr_ = true;
    m->subject = parseExpr();
    noBraceExpr_ = false;
    expect(Tok::LBrace, "to start the match arms");
    NewlineGuard g(*this, false);
    std::vector<MatchArm> arms;
    while (!at(Tok::RBrace) && !at(Tok::Eof)) {
        std::size_t before = pos_;
        MatchArm arm{};
        arm.pattern = parsePattern();
        if (accept(Tok::KwIf)) {
            arm.guard = parseExpr();
        }
        expect(Tok::FatArrow, "after the match pattern");
        arm.body = parseArmBody();
        arms.push_back(arm);
        if (!accept(Tok::Comma) && !at(Tok::RBrace) && !cur().newlineBefore) {
            error(cur().span, "expected a newline or ',' between match arms");
            syncTo({Tok::RBrace});
        }
        if (pos_ == before) {
            advance();
        }
    }
    expect(Tok::RBrace, "to close the match");
    m->arms = list(arms);
    m->span = begin.to(prevSpan());
    return m;
}

Expr* Parser::parsePrimary() {
    const Token& t = cur();
    Span span = t.span;
    switch (t.kind) {
    case Tok::Int: {
        auto* e = node<IntLitExpr>(span);
        e->kind = ExprKind::IntLit;
        e->value = advance().intValue;
        return e;
    }
    case Tok::Float: {
        auto* e = node<FloatLitExpr>(span);
        e->kind = ExprKind::FloatLit;
        e->value = advance().floatValue;
        return e;
    }
    case Tok::Char: {
        auto* e = node<CharLitExpr>(span);
        e->kind = ExprKind::CharLit;
        e->value = static_cast<std::uint32_t>(advance().intValue);
        return e;
    }
    case Tok::String: {
        auto* e = node<StringLitExpr>(span);
        e->kind = ExprKind::StringLit;
        e->value = advance().text;
        return e;
    }
    case Tok::InterpBegin: return parseInterp();
    case Tok::KwTrue:
    case Tok::KwFalse: {
        auto* e = node<BoolLitExpr>(span);
        e->kind = ExprKind::BoolLit;
        e->value = advance().kind == Tok::KwTrue;
        return e;
    }
    case Tok::KwNull: {
        advance();
        auto* e = node<NullLitExpr>(span);
        e->kind = ExprKind::NullLit;
        return e;
    }
    case Tok::KwSelf: {
        advance();
        auto* e = node<SelfExpr>(span);
        e->kind = ExprKind::SelfRef;
        return e;
    }
    case Tok::Ident: {
        auto* e = node<IdentExpr>(span);
        e->kind = ExprKind::Ident;
        e->name = names_.intern(advance().text);
        if (at(Tok::ColonColon) && peekTok().kind == Tok::Lt) {
            advance();
            e->typeArgs = parseTypeArgs();
            e->span = span.to(prevSpan());
        }
        return e;
    }
    case Tok::LParen: return parseParenOrLambda();
    case Tok::LBracket: {
        advance();
        NewlineGuard g(*this, true);
        std::vector<Expr*> elems;
        if (!at(Tok::RBracket)) {
            Expr* first = parseExpr();
            if (accept(Tok::Semi)) {
                auto* r = node<ListRepeatExpr>(span);
                r->kind = ExprKind::ListRepeat;
                r->value = first;
                r->count = parseExpr();
                expect(Tok::RBracket, "to close the list");
                r->span = span.to(prevSpan());
                return r;
            }
            elems.push_back(first);
            while (accept(Tok::Comma) && !at(Tok::RBracket)) {
                elems.push_back(parseExpr());
            }
        }
        expect(Tok::RBracket, "to close the list");
        auto* l = node<ListLitExpr>(span.to(prevSpan()));
        l->kind = ExprKind::ListLit;
        l->elems = list(elems);
        return l;
    }
    case Tok::KwIf: return parseIf();
    case Tok::KwMatch: return parseMatch();
    case Tok::LBrace: return parseBlock();
    default: break;
    }
    error(span, std::format("expected an expression, found {}", tokName(t.kind)));
    if (!at(Tok::RBrace) && !at(Tok::Eof) && !at(Tok::RParen) && !at(Tok::RBracket)) {
        advance();
    }
    auto* e = node<NullLitExpr>(span);
    e->kind = ExprKind::NullLit;
    return e;
}

bool Parser::closeAngle() {
    if (accept(Tok::Gt)) {
        return true;
    }
    error(cur().span, std::format("expected '>' to close the type arguments, found {}", tokName(cur().kind)));
    return false;
}

std::span<TypeExpr*> Parser::parseTypeArgs() {
    expect(Tok::Lt, "to start the type arguments");
    std::vector<TypeExpr*> args;
    while (!at(Tok::Gt) && !at(Tok::Eof)) {
        args.push_back(parseType());
        if (!accept(Tok::Comma)) {
            break;
        }
    }
    closeAngle();
    return list(args);
}

TypeExpr* Parser::parseType() {
    DepthGuard guard(*this);
    Span begin = cur().span;
    TypeExpr* result = nullptr;
    if (tooDeep()) {
        auto* n = node<NamedTypeExpr>(begin);
        n->kind = TypeExprKind::Named;
        return n;
    }
    if (accept(Tok::LBracket)) {
        TypeExpr* elem = parseType();
        if (accept(Tok::Semi)) {
            auto* a = node<ArrayTypeExpr>(begin);
            a->kind = TypeExprKind::Array;
            a->elem = elem;
            a->size = parseExpr();
            result = a;
        } else {
            auto* l = node<ListTypeExpr>(begin);
            l->kind = TypeExprKind::List;
            l->elem = elem;
            result = l;
        }
        expect(Tok::RBracket, "to close the list type");
    } else if (accept(Tok::LParen)) {
        std::vector<TypeExpr*> elems;
        while (!at(Tok::RParen) && !at(Tok::Eof)) {
            elems.push_back(parseType());
            if (!accept(Tok::Comma)) {
                break;
            }
        }
        expect(Tok::RParen, "to close the tuple type");
        if (at(Tok::Arrow)) {
            error(cur().span, "function types are written 'fn(A, B) -> R'");
            advance();
            parseType();
        }
        auto* t = node<TupleTypeExpr>(begin);
        t->kind = TypeExprKind::Tuple;
        t->elems = list(elems);
        result = t;
    } else if (accept(Tok::KwFn)) {
        expect(Tok::LParen, "after 'fn' in a function type");
        std::vector<TypeExpr*> params;
        {
            NewlineGuard g(*this, true);
            while (!at(Tok::RParen) && !at(Tok::Eof)) {
                params.push_back(parseType());
                if (!accept(Tok::Comma)) {
                    break;
                }
            }
            expect(Tok::RParen, "to close the function type");
        }
        auto* f = node<FnTypeExpr>(begin);
        f->kind = TypeExprKind::Fn;
        f->params = list(params);
        f->result = accept(Tok::Arrow) ? parseType() : nullptr;
        result = f;
    } else if (at(Tok::Ident) || at(Tok::KwSelf)) {
        auto* n = node<NamedTypeExpr>(begin);
        n->kind = TypeExprKind::Named;
        if (at(Tok::KwSelf)) {
            advance();
            std::vector<Symbol> path{names_.intern("Self")};
            n->path = list(path);
        } else {
            n->path = parsePath();
        }
        if (at(Tok::Lt)) {
            n->args = parseTypeArgs();
        }
        result = n;
    } else {
        error(cur().span, std::format("expected a type, found {}", tokName(cur().kind)));
        auto* n = node<NamedTypeExpr>(begin);
        n->kind = TypeExprKind::Named;
        return n;
    }
    result->span = begin.to(prevSpan());
    while (at(Tok::Question) && !cur().newlineBefore) {
        advance();
        auto* q = node<NullableTypeExpr>(begin.to(prevSpan()));
        q->kind = TypeExprKind::Nullable;
        q->inner = result;
        result = q;
    }
    return result;
}

} // namespace quartz::syntax
