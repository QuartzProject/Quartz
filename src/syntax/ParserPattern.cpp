#include "syntax/ParserImpl.hpp"

namespace quartz::syntax {

Pattern* Parser::parsePattern() {
    Span begin = cur().span;
    Pattern* first = parsePrimaryPattern();
    if (!at(Tok::Pipe)) {
        return first;
    }
    std::vector<Pattern*> alts{first};
    while (accept(Tok::Pipe)) {
        alts.push_back(parsePrimaryPattern());
    }
    auto* p = arena_.make<Pattern>();
    p->kind = PatternKind::Or;
    p->sub = list(alts);
    p->span = begin.to(prevSpan());
    return p;
}

Pattern* Parser::parsePrimaryPattern() {
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

} // namespace quartz::syntax
