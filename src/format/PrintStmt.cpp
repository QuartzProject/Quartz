#include "format/Printer.hpp"

namespace quartz::format {

void Printer::block(const BlockExpr* b) {
    flush(b->span.begin);
    std::uint32_t closeAt = b->span.end - 1;
    text("{");
    if (b->stmts.empty() && !commentBefore(closeAt)) {
        text("}");
        return;
    }
    {
        Indent in(*this);
        for (const Stmt* s : b->stmts) {
            newline(s->span.begin);
            flush(s->span.begin);
            if (blankLineBefore(s->span.begin)) {
                blankLine();
            }
            Scope scope(*this);
            stmt(s);
        }
        newline(closeAt);
        flush(closeAt);
    }
    newline(closeAt);
    text("}");
}

void Printer::assignOp(AssignOp op) {
    switch (op) {
    case AssignOp::Set:
        text("=");
        return;
    case AssignOp::Add:
        text("+=");
        return;
    case AssignOp::Sub:
        text("-=");
        return;
    case AssignOp::Mul:
        text("*=");
        return;
    case AssignOp::Div:
        text("/=");
        return;
    case AssignOp::Rem:
        text("%=");
        return;
    case AssignOp::BitAnd:
        text("&=");
        return;
    case AssignOp::BitOr:
        text("|=");
        return;
    case AssignOp::BitXor:
        text("^=");
        return;
    case AssignOp::Shl:
        text("<<=");
        return;
    case AssignOp::Shr:
        text(">>=");
        return;
    }
}

void Printer::stmt(const Stmt* s) {
    auto keywordBlock = [&](const char* kw, const Expr* head, const BlockExpr* body) {
        text(kw);
        space();
        if (head != nullptr) {
            expr(head);
            space();
        }
        block(body);
    };
    switch (s->kind) {
    case StmtKind::Expr:
        expr(static_cast<const ExprStmt*>(s)->expr);
        return;
    case StmtKind::Let: {
        const auto* l = static_cast<const LetStmt*>(s);
        text(l->isConst ? "const" : l->isRef ? "ref" : l->mutable_ ? "var" : "let");
        space();
        text(name(l->name));
        if (l->type != nullptr) {
            text(":");
            space();
            type(l->type);
        }
        if (l->init != nullptr) {
            space();
            text("=");
            space();
            expr(l->init);
        }
        return;
    }
    case StmtKind::Assign: {
        const auto* a = static_cast<const AssignStmt*>(s);
        expr(a->target);
        space();
        assignOp(a->op);
        space();
        expr(a->value);
        return;
    }
    case StmtKind::While: {
        const auto* w = static_cast<const WhileStmt*>(s);
        keywordBlock("while", w->cond, w->body);
        return;
    }
    case StmtKind::For: {
        const auto* f = static_cast<const ForStmt*>(s);
        text("for");
        space();
        if (f->indexName.valid()) {
            text(name(f->indexName));
            text(",");
            space();
        }
        text(name(f->name));
        space();
        text("in");
        space();
        expr(f->iterable);
        space();
        block(f->body);
        return;
    }
    case StmtKind::Return: {
        text("return");
        if (const Expr* v = static_cast<const ReturnStmt*>(s)->value) {
            space();
            expr(v);
        }
        return;
    }
    case StmtKind::Break:
        text("break");
        return;
    case StmtKind::Continue:
        text("continue");
        return;
    case StmtKind::Scope:
        keywordBlock("scope", nullptr, static_cast<const ScopeStmt*>(s)->body);
        return;
    case StmtKind::Every: {
        const auto* e = static_cast<const EveryStmt*>(s);
        keywordBlock("every", e->interval, e->body);
        return;
    }
    case StmtKind::At: {
        const auto* a = static_cast<const AtStmt*>(s);
        keywordBlock("at", a->context, a->body);
        return;
    }
    }
}

void Printer::ifExpr(const IfExpr* e) {
    text("if");
    space();
    if (e->cond != nullptr) {
        expr(e->cond);
    } else {
        text("let");
        space();
        text(name(e->bindName));
        space();
        text("=");
        space();
        expr(e->bindValue);
    }
    space();
    block(e->then);
    if (e->otherwise == nullptr) {
        return;
    }
    std::size_t kw = nextToken(e->then->span.end, Tok::KwElse);
    std::uint32_t elseAt = kw < toks_.size() ? toks_[kw].span.begin : e->then->span.end;
    if (commentBefore(elseAt)) {
        newline(elseAt);
        flush(elseAt);
    }
    space();
    text("else");
    space();
    if (e->otherwise->kind == ExprKind::If) {
        flush(e->otherwise->span.begin);
        ifExpr(static_cast<const IfExpr*>(e->otherwise));
    } else {
        block(static_cast<const BlockExpr*>(e->otherwise));
    }
}

void Printer::armBody(const Expr* body) {
    if (body->kind != ExprKind::Block) {
        expr(body);
        return;
    }
    const auto* b = static_cast<const BlockExpr*>(body);
    if (src_[b->span.begin] == '{') {
        block(b);
        return;
    }
    // The parser wraps a statement arm body (`x => y = 1`) in a block that has no braces in the source.
    flush(b->span.begin);
    stmt(b->stmts[0]);
}

void Printer::match(const MatchExpr* m) {
    text("match");
    space();
    expr(m->subject);
    space();
    text("{");
    std::uint32_t closeAt = m->span.end - 1;
    if (m->arms.empty() && !commentBefore(closeAt)) {
        text("}");
        return;
    }
    {
        Indent in(*this);
        for (const MatchArm& arm : m->arms) {
            std::uint32_t begin = arm.pattern->span.begin;
            newline(begin);
            flush(begin);
            if (blankLineBefore(begin)) {
                blankLine();
            }
            Scope scope(*this);
            pattern(arm.pattern);
            if (arm.guard != nullptr) {
                space();
                text("if");
                space();
                expr(arm.guard);
            }
            space();
            text("=>");
            space();
            armBody(arm.body);
        }
        newline(closeAt);
        flush(closeAt);
    }
    newline(closeAt);
    text("}");
}

void Printer::pattern(const Pattern* p, std::size_t fence) {
    flush(p->span.begin);
    switch (p->kind) {
    case PatternKind::Wildcard:
        text("_");
        return;
    case PatternKind::Binding:
        text(name(p->name));
        return;
    case PatternKind::Literal:
        expr(p->literal, kPrecUnary, fence);
        return;
    case PatternKind::Range: {
        expr(p->literal, kPrecUnary, fence);
        std::size_t op = tokenEndingAt(outer(p->literal, fence).end) + 1;
        text(op < toks_.size() && toks_[op].kind == Tok::DotDotEq ? "..=" : "..");
        expr(p->rangeHi, kPrecUnary, fence);
        return;
    }
    case PatternKind::Variant: {
        for (std::size_t i = 0; i < p->path.size(); ++i) {
            if (i > 0) {
                text(".");
            }
            text(name(p->path[i]));
        }
        if (p->sub.empty() && src_[p->span.end - 1] != ')') {
            return;
        }
        std::size_t open = nextToken(p->span.begin, Tok::LParen);
        items(
            "(", ")", toks_[open].span.end, p->span.end - 1, p->sub.size(),
            [&](std::size_t i) { return p->sub[i]->span.begin; }, [&](std::size_t i) { return p->sub[i]->span.end; },
            [&](std::size_t i) { pattern(p->sub[i], open); });
        return;
    }
    }
}

} // namespace quartz::format
