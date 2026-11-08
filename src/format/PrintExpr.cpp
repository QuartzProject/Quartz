#include "format/Printer.hpp"

namespace quartz::format {

namespace {

int binaryPrec(BinaryOp op) {
    switch (op) {
    case BinaryOp::Or:
        return kPrecOr;
    case BinaryOp::And:
        return kPrecAnd;
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
        return kPrecCompare;
    case BinaryOp::Coalesce:
        return kPrecCoalesce;
    case BinaryOp::BitOr:
        return kPrecBitOr;
    case BinaryOp::BitXor:
        return kPrecBitXor;
    case BinaryOp::BitAnd:
        return kPrecBitAnd;
    case BinaryOp::Shl:
    case BinaryOp::Shr:
        return kPrecShift;
    case BinaryOp::Add:
    case BinaryOp::Sub:
        return kPrecAdd;
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Rem:
        return kPrecMul;
    }
    return kPrecLowest;
}

const char* unaryText(UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg:
        return "-";
    case UnaryOp::Not:
        return "!";
    case UnaryOp::BitNot:
        return "~";
    }
    return "";
}

} // namespace

int Printer::precOf(const Expr* e) {
    switch (e->kind) {
    case ExprKind::Binary:
        return binaryPrec(static_cast<const BinaryExpr*>(e)->op);
    case ExprKind::Range:
        return kPrecRange;
    case ExprKind::Cast:
    case ExprKind::Is:
    case ExprKind::With:
        return kPrecCast;
    case ExprKind::Unary:
        return kPrecUnary;
    case ExprKind::Wait:
    case ExprKind::Lambda:
        return kPrecLowest;
    case ExprKind::Call:
    case ExprKind::Member:
    case ExprKind::Index:
    case ExprKind::Propagate:
        return kPrecPostfix;
    default:
        return kPrecPrimary;
    }
}

Extent Printer::extent(const Expr* e) const {
    switch (e->kind) {
    case ExprKind::Binary: {
        const auto* b = static_cast<const BinaryExpr*>(e);
        return {outer(b->lhs).begin, outer(b->rhs).end, 0};
    }
    case ExprKind::Unary:
        return {e->span.begin, outer(static_cast<const UnaryExpr*>(e)->operand).end, 0};
    case ExprKind::Range:
        return {outer(static_cast<const RangeExpr*>(e)->lo).begin, e->span.end, 0};
    case ExprKind::Cast:
        return {outer(static_cast<const CastExpr*>(e)->value).begin, e->span.end, 0};
    case ExprKind::Is: {
        const auto* is = static_cast<const IsExpr*>(e);
        return is->letForm ? Extent{e->span.begin, outer(is->value).end, 0}
                           : Extent{outer(is->value).begin, e->span.end, 0};
    }
    case ExprKind::With:
        return {outer(static_cast<const WithExpr*>(e)->value).begin, e->span.end, 0};
    case ExprKind::Member:
        return {outer(static_cast<const MemberExpr*>(e)->object).begin, e->span.end, 0};
    case ExprKind::Call:
        return {outer(static_cast<const CallExpr*>(e)->callee).begin, e->span.end, 0};
    case ExprKind::Index:
        return {outer(static_cast<const IndexExpr*>(e)->object).begin, e->span.end, 0};
    case ExprKind::Propagate:
        return {outer(static_cast<const PropagateExpr*>(e)->inner).begin, e->span.end, 0};
    default:
        return {e->span.begin, e->span.end, 0};
    }
}

Extent Printer::outer(const Expr* e, std::size_t fence) const {
    Extent x = extent(e);
    std::size_t i0 = tokenAt(x.begin);
    std::size_t i1 = tokenEndingAt(x.end);
    if (i0 >= toks_.size() || i1 >= toks_.size()) {
        return x;
    }
    while (i0 > 0 && i1 + 1 < toks_.size() && i0 - 1 != fence && toks_[i0 - 1].kind == Tok::LParen &&
           toks_[i1 + 1].kind == Tok::RParen && matching(i0 - 1) == i1 + 1) {
        --i0;
        ++i1;
        ++x.parens;
    }
    x.begin = toks_[i0].span.begin;
    x.end = toks_[i1].span.end;
    return x;
}

void Printer::expr(const Expr* e, int minPrec, std::size_t fence) {
    Extent x = outer(e, fence);
    flush(x.begin);
    bool parens = x.parens > 0 || precOf(e) < minPrec;
    if (!parens) {
        exprInner(e);
        return;
    }
    text("(");
    exprInner(e);
    if (x.parens > 0) {
        flush(x.end - 1);
    }
    text(")");
}

void Printer::exprList(std::string_view open, std::string_view close, std::uint32_t openEnd, std::uint32_t closeAt,
                       std::span<Expr* const> elems, bool singleComma) {
    items(
        open, close, openEnd, closeAt, elems.size(), [&](std::size_t i) { return outer(elems[i]).begin; },
        [&](std::size_t i) { return outer(elems[i]).end; }, [&](std::size_t i) { expr(elems[i]); }, singleComma);
}

void Printer::binary(const BinaryExpr* b) {
    int p = binaryPrec(b->op);
    int lp = p;
    int rp = p + 1;
    if (b->op == BinaryOp::Coalesce) {
        lp = p + 1;
        rp = p;
    } else if (p == kPrecCompare) {
        lp = p + 1;
    }
    expr(b->lhs, lp);
    space();
    text(binaryOpText(b->op));
    Extent r = outer(b->rhs);
    if (newlineIn(outer(b->lhs).end, r.begin)) {
        contBreak(r.begin);
    } else {
        space();
    }
    expr(b->rhs, rp);
}

void Printer::call(const CallExpr* c) {
    expr(c->callee, kPrecPostfix);
    std::size_t open = nextToken(outer(c->callee).end, Tok::LParen);
    auto argBegin = [&](std::size_t i) {
        std::uint32_t b = outer(c->args[i].value, open).begin;
        std::size_t t = tokenAt(b);
        if (c->args[i].name.valid() && t >= 2 && t < toks_.size()) {
            return toks_[t - 2].span.begin;
        }
        return b;
    };
    items(
        "(", ")", toks_[open].span.end, c->span.end - 1, c->args.size(), argBegin,
        [&](std::size_t i) { return outer(c->args[i].value, open).end; },
        [&](std::size_t i) {
            const Arg& a = c->args[i];
            if (a.name.valid()) {
                flush(argBegin(i));
                text(name(a.name));
                space();
                text("=");
                space();
            }
            expr(a.value, kPrecLowest, open);
        });
}

// One line as `{ a = 1, b = 2 }`, or one field per line without commas when the source breaks after the brace.
void Printer::with(const WithExpr* w) {
    expr(w->value, kPrecCast);
    space();
    text("with");
    space();
    std::size_t open = nextToken(outer(w->value).end, Tok::LBrace);
    std::uint32_t openEnd = toks_[open].span.end;
    std::uint32_t closeAt = w->span.end - 1;
    auto fieldBegin = [&](std::size_t i) {
        std::size_t t = tokenAt(outer(w->fields[i].value, open).begin);
        return t >= 2 && t < toks_.size() ? toks_[t - 2].span.begin : w->fields[i].value->span.begin;
    };
    auto field = [&](std::size_t i) {
        Scope sc(*this);
        flush(fieldBegin(i));
        text(name(w->fields[i].name));
        space();
        text("=");
        space();
        expr(w->fields[i].value, kPrecLowest, open);
    };
    flush(openEnd - 1);
    text("{");
    bool multi = !w->fields.empty() && newlineIn(openEnd, fieldBegin(0));
    if (!multi) {
        for (std::size_t i = 0; i < w->fields.size(); ++i) {
            text(i > 0 ? "," : "");
            space();
            field(i);
        }
        flush(closeAt);
        text(w->fields.empty() ? "}" : " }");
        return;
    }
    {
        Indent in(*this);
        for (std::size_t i = 0; i < w->fields.size(); ++i) {
            newline(fieldBegin(i));
            field(i);
        }
        newline(closeAt);
        flush(closeAt);
    }
    newline(closeAt);
    text("}");
}

void Printer::interp(const InterpExpr* e) {
    std::size_t first = tokenAt(e->span.begin);
    std::vector<std::size_t> parts;
    int depth = 0;
    for (std::size_t i = first + 1; i < toks_.size(); ++i) {
        Tok k = toks_[i].kind;
        if (k == Tok::InterpBegin) {
            ++depth;
        } else if (k == Tok::FormatSpec && depth == 0) {
            parts.push_back(i); // printed with the part after it
        } else if (k == Tok::InterpMid && depth == 0) {
            parts.push_back(i);
        } else if (k == Tok::InterpEnd) {
            if (depth == 0) {
                parts.push_back(i);
                break;
            }
            --depth;
        }
    }
    text(raw(toks_[first].span.begin, toks_[first].span.end));
    std::size_t p = 0;
    for (std::size_t k = 0; k < e->exprs.size(); ++k) {
        expr(e->exprs[k]);
        if (p < parts.size() && toks_[parts[p]].kind == Tok::FormatSpec) {
            const Span& s = toks_[parts[p++]].span;
            flush(s.begin);
            text(raw(s.begin, s.end));
        }
        if (p < parts.size()) {
            const Span& s = toks_[parts[p++]].span;
            flush(s.begin);
            text(raw(s.begin, s.end));
        }
    }
}

void Printer::lambda(const LambdaExpr* l) {
    std::size_t open = tokenAt(l->span.begin);
    std::size_t close = matching(open);
    items(
        "(", ")", toks_[open].span.end, toks_[close].span.begin, l->params.size(),
        [&](std::size_t i) { return l->params[i].span.begin; }, [&](std::size_t i) { return paramEnd(l->params[i]); },
        [&](std::size_t i) { param(l->params[i]); });
    space();
    text("=>");
    space();
    if (l->body->kind == ExprKind::Block) {
        block(static_cast<const BlockExpr*>(l->body));
    } else {
        expr(l->body);
    }
}

void Printer::exprInner(const Expr* e) {
    switch (e->kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::CharLit:
    case ExprKind::StringLit:
        text(raw(e->span.begin, e->span.end));
        return;
    case ExprKind::BoolLit:
        text(static_cast<const BoolLitExpr*>(e)->value ? "true" : "false");
        return;
    case ExprKind::NullLit:
        text("null");
        return;
    case ExprKind::SelfRef:
        text("self");
        return;
    case ExprKind::Interp:
        interp(static_cast<const InterpExpr*>(e));
        return;
    case ExprKind::Ident: {
        const auto* id = static_cast<const IdentExpr*>(e);
        text(name(id->name));
        if (!id->typeArgs.empty()) {
            text("::");
            typeArgs(id->typeArgs, e->span.end - 1);
        }
        return;
    }
    case ExprKind::Unary: {
        const auto* u = static_cast<const UnaryExpr*>(e);
        text(unaryText(u->op));
        if (u->op == UnaryOp::Neg && u->operand->kind == ExprKind::Unary &&
            static_cast<const UnaryExpr*>(u->operand)->op == UnaryOp::Neg && outer(u->operand).parens == 0) {
            space();
        }
        expr(u->operand, kPrecUnary);
        return;
    }
    case ExprKind::Binary:
        binary(static_cast<const BinaryExpr*>(e));
        return;
    case ExprKind::Call:
        call(static_cast<const CallExpr*>(e));
        return;
    case ExprKind::Member: {
        const auto* m = static_cast<const MemberExpr*>(e);
        // `x?.y` would lex as a safe member access, so a propagated object keeps its parentheses.
        expr(m->object, m->object->kind == ExprKind::Propagate ? kPrecPrimary : kPrecPostfix);
        std::size_t last = tokenEndingAt(e->span.end);
        std::uint32_t dot = last > 0 && last < toks_.size() ? toks_[last - 1].span.begin : e->span.end;
        if (newlineIn(outer(m->object).end, dot)) {
            contBreak(dot);
            flush(dot);
        }
        text(m->safe ? "?." : ".");
        if (last < toks_.size()) {
            flush(toks_[last].span.begin);
        }
        text(name(m->name));
        return;
    }
    case ExprKind::Index: {
        const auto* ix = static_cast<const IndexExpr*>(e);
        expr(ix->object, kPrecPostfix);
        text("[");
        expr(ix->index);
        flush(e->span.end - 1);
        text("]");
        return;
    }
    case ExprKind::Propagate: {
        const auto* p = static_cast<const PropagateExpr*>(e);
        expr(p->inner, p->inner->kind == ExprKind::Propagate ? kPrecPrimary : kPrecPostfix);
        text("?");
        return;
    }
    case ExprKind::Range: {
        const auto* r = static_cast<const RangeExpr*>(e);
        expr(r->lo, kPrecRange);
        text(r->inclusive ? "..=" : "..");
        expr(r->hi, kPrecRange + 1);
        return;
    }
    case ExprKind::ListLit: {
        exprList("[", "]", e->span.begin + 1, e->span.end - 1, static_cast<const ListLitExpr*>(e)->elems, false);
        return;
    }
    case ExprKind::ListRepeat: {
        const auto* r = static_cast<const ListRepeatExpr*>(e);
        text("[");
        expr(r->value);
        text(";");
        space();
        expr(r->count);
        flush(e->span.end - 1);
        text("]");
        return;
    }
    case ExprKind::TupleLit: {
        exprList("(", ")", e->span.begin + 1, e->span.end - 1, static_cast<const TupleLitExpr*>(e)->elems, true);
        return;
    }
    case ExprKind::Lambda:
        lambda(static_cast<const LambdaExpr*>(e));
        return;
    case ExprKind::Cast:
        expr(static_cast<const CastExpr*>(e)->value, kPrecCast);
        space();
        text("as");
        space();
        type(static_cast<const CastExpr*>(e)->target);
        return;
    case ExprKind::Is: {
        const auto* is = static_cast<const IsExpr*>(e);
        if (is->letForm) {
            text("let");
            space();
            pattern(is->pattern);
            space();
            text("=");
            space();
            expr(is->value);
            return;
        }
        expr(is->value, kPrecCast);
        space();
        text("is");
        space();
        pattern(is->pattern);
        return;
    }
    case ExprKind::With:
        with(static_cast<const WithExpr*>(e));
        return;
    case ExprKind::Wait:
        text("wait");
        space();
        expr(static_cast<const WaitExpr*>(e)->value, kPrecRange);
        return;
    case ExprKind::Jump: {
        const auto* j = static_cast<const JumpExpr*>(e);
        text(j->stmt->kind == StmtKind::Return ? "return" : j->stmt->kind == StmtKind::Break ? "break" : "continue");
        if (j->value != nullptr) {
            space();
            expr(j->value, kPrecLowest);
        }
        return;
    }
    case ExprKind::If:
        ifExpr(static_cast<const IfExpr*>(e));
        return;
    case ExprKind::Match:
        match(static_cast<const MatchExpr*>(e));
        return;
    case ExprKind::Block:
        block(static_cast<const BlockExpr*>(e));
        return;
    }
}

} // namespace quartz::format
