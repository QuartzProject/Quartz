#include "ir/LowerImpl.hpp"

namespace quartz::ir {

// Tests and binds in one pass: a failed test branches to `fail`, so names are only read after every test passed.
void Lowerer::matchPattern(Pattern* p, Reg subject, TypeId type, std::size_t fail) {
    const TypeInfo& info = types_->info(type);
    auto failUnless = [&](Reg ok) {
        Reg miss = newReg(kBool);
        emit(Op::Not, miss, ok);
        brIf(miss, fail);
    };
    if (info.kind == TypeKind::Nullable && p->kind != PatternKind::Wildcard && p->kind != PatternKind::Binding &&
        p->kind != PatternKind::Or) {
        TypeId inner = info.elem;
        Reg isNull = newReg(kBool);
        emit(Op::IsNull, isNull, subject);
        if (p->kind == PatternKind::Literal && p->literal->kind == ExprKind::NullLit) {
            failUnless(isNull);
            return;
        }
        brIf(isNull, fail);
        Reg value = newReg(inner);
        emit(Op::Unwrap, value, subject);
        matchPattern(p, value, inner, fail);
        return;
    }
    switch (p->kind) {
    case PatternKind::Wildcard:
        return;
    case PatternKind::Binding:
        if (orBound_.contains(p->local)) {
            emit(Op::Move, p->local->reg, subject);
            return;
        }
        if (p->local != nullptr) {
            p->local->reg = newReg(type);
            p->local->scopeDepth = static_cast<std::uint32_t>(scopes_.size());
            emit(Op::Move, p->local->reg, subject);
            nameLocal(p->local);
        }
        return;
    case PatternKind::Literal: {
        Reg c = newReg(kBool);
        emit(Op::Eq, c, subject, lowerExpr(p->literal, type));
        failUnless(c);
        return;
    }
    case PatternKind::Range: {
        Reg lo = newReg(kBool);
        emit(Op::Ge, lo, subject, lowerExpr(p->literal, type));
        failUnless(lo);
        Reg hi = newReg(kBool);
        emit(p->inclusive ? Op::Le : Op::Lt, hi, subject, lowerExpr(p->rangeHi, type));
        failUnless(hi);
        return;
    }
    case PatternKind::Variant: {
        Reg tag = newReg(kI64);
        emit(Op::EnumTag, tag, subject);
        Reg is = newReg(kBool);
        emit(Op::Eq, is, tag, constInt(p->variantIndex, kI64));
        failUnless(is);
        std::vector<TypeId> payload = info.variants[p->variantIndex].payload;
        for (std::uint32_t i = 0; i < p->sub.size(); ++i) {
            if (p->sub[i]->kind == PatternKind::Wildcard) {
                continue;
            }
            Reg field = newReg(payload[i]);
            enumField(field, subject, type, p->variantIndex, i);
            matchPattern(p->sub[i], field, payload[i], fail);
        }
        return;
    }
    case PatternKind::Or:
        matchOr(p, subject, type, fail);
        return;
    case PatternKind::Tuple:
    case PatternKind::Struct: {
        std::vector<TypeId> fields = info.elems;
        if (info.kind == TypeKind::Struct) {
            fields.clear();
            for (const Field& f : info.fields) {
                fields.push_back(f.type);
            }
        }
        for (std::uint32_t i = 0; i < p->sub.size(); ++i) {
            if (p->sub[i]->kind == PatternKind::Wildcard) {
                continue;
            }
            Reg field = newReg(fields[i]);
            emit(Op::FieldGet, field, subject, kNoReg, i);
            matchPattern(p->sub[i], field, fields[i], fail);
        }
        return;
    }
    }
}

namespace {

void bindingsOf(Pattern* p, std::vector<Pattern*>& out) {
    if (p->kind == PatternKind::Binding && p->local != nullptr) {
        out.push_back(p);
    }
    for (Pattern* s : p->sub) {
        bindingsOf(s, out);
    }
}

// A variant of a one-variant enum still emits a tag test, which the validator sees as a way around the bindings.
bool testsSomething(const Pattern* p) {
    if (p->kind == PatternKind::Variant || p->kind == PatternKind::Literal || p->kind == PatternKind::Range ||
        p->kind == PatternKind::Or) {
        return true;
    }
    for (const Pattern* s : p->sub) {
        if (testsSomething(s)) {
            return true;
        }
    }
    return false;
}

} // namespace

// Every alternative writes the same registers, so the names are bound on each path that reaches the end.
void Lowerer::matchOr(Pattern* p, Reg subject, TypeId type, std::size_t fail) {
    std::vector<Pattern*> binds;
    bindingsOf(p, binds);
    std::vector<sema::Local*> fresh;
    for (Pattern* b : binds) {
        if (!orBound_.contains(b->local)) {
            b->local->reg = newReg(b->local->type);
            b->local->scopeDepth = static_cast<std::uint32_t>(scopes_.size());
            orBound_.insert(b->local);
            fresh.push_back(b->local);
        }
    }
    std::size_t matched = depth_;
    open(Op::Block);
    for (std::size_t i = 0; i + 1 < p->sub.size(); ++i) {
        std::size_t next = depth_;
        open(Op::Block);
        matchPattern(p->sub[i], subject, type, next);
        br(matched);
        close();
    }
    matchPattern(p->sub.back(), subject, type, fail);
    close();
    for (sema::Local* l : fresh) {
        orBound_.erase(l);
        nameLocal(l);
    }
}

Reg Lowerer::lowerIs(IsExpr* e) {
    Reg v = lowerExpr(e->value);
    Reg res = newReg(kBool);
    emit(Op::Move, res, constInt(0, kBool));
    std::size_t miss = depth_;
    open(Op::Block);
    matchPattern(e->pattern, v, T(e->value->type), miss);
    emit(Op::Move, res, constInt(1, kBool));
    close();
    return res;
}

void Lowerer::lowerLetPattern(LetStmt* s) {
    TypeId t = T(s->init->type);
    if (s->type != nullptr) {
        t = T(s->type->type);
    }
    Reg v = lowerExpr(s->init, t);
    if (!testsSomething(s->pattern)) {
        std::size_t never = depth_;
        open(Op::Block);
        matchPattern(s->pattern, v, t, never);
        close();
        return;
    }
    // Sema proved the pattern cannot fail; the trap after it keeps every path past the `let` through the bindings.
    std::size_t bound = depth_;
    open(Op::Block);
    std::size_t never = depth_;
    open(Op::Block);
    matchPattern(s->pattern, v, t, never);
    br(bound);
    close();
    emit(Op::Trap, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(TrapCode::Unreachable));
    close();
}

Reg Lowerer::lowerArmBody(Expr* body, TypeId want, bool wantValue) {
    if (wantValue) {
        return lowerExpr(body, want);
    }
    if (body->kind == ExprKind::Block) {
        lowerBlockStmts(static_cast<BlockExpr*>(body));
    } else if (body->kind == ExprKind::If) {
        lowerIf(static_cast<IfExpr*>(body), kError, false);
    } else if (body->kind == ExprKind::Match) {
        lowerMatch(static_cast<MatchExpr*>(body), kError, false);
    } else {
        lowerExpr(body);
    }
    return kNoReg;
}

Reg Lowerer::lowerMatch(MatchExpr* m, TypeId want, bool wantValue) {
    TypeId st = T(m->subject->type);
    Reg subject = lowerExpr(m->subject);
    TypeId rt = wantValue ? (want != kError ? want : T(m->type)) : kError;
    Reg result = wantValue && rt != kVoid && rt != kNever ? newReg(rt) : kNoReg;
    std::size_t exit = depth_;
    open(Op::Block);
    for (std::size_t a = 0; a < m->arms.size(); ++a) {
        MatchArm& arm = m->arms[a];
        if (a == m->narrowFrom && types_->kind(st) == TypeKind::Nullable) {
            // An earlier arm took null, so the rest match the value inside.
            st = types_->info(st).elem;
            Reg inner = newReg(st);
            emit(Op::Unwrap, inner, subject);
            subject = inner;
        }
        std::size_t next = depth_;
        open(Op::Block);
        matchPattern(arm.pattern, subject, st, next);
        if (arm.guard != nullptr) {
            Reg g = lowerExpr(arm.guard, kBool);
            Reg miss = newReg(kBool);
            emit(Op::Not, miss, g);
            brIf(miss, next);
        }
        Reg v = lowerArmBody(arm.body, rt, wantValue);
        if (result != kNoReg && v != kNoReg && T(arm.body->type) != kNever) {
            emit(Op::Move, result, v);
        }
        br(exit);
        close();
    }
    emit(Op::Trap, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(TrapCode::Unreachable));
    close();
    return result;
}

} // namespace quartz::ir
