#include "ir/LowerImpl.hpp"

#include <format>

namespace quartz::ir {

bool Lowerer::signatureHasFn(const Function& f) const {
    for (std::uint32_t p = 0; p < f.paramCount; ++p) {
        if (types_->containsFn(f.regs[p])) {
            return true;
        }
    }
    return types_->containsFn(f.result);
}

Reg Lowerer::lowerFunctionValue(IdentExpr* e) {
    sema::FnInfo* fn = e->fn;
    std::uint32_t target = fn->index;
    if (fn->generic()) {
        std::vector<TypeId> args;
        for (std::uint32_t a : e->fnTypeArgs) {
            args.push_back(T(a));
        }
        target = instance(fn, args);
    }
    Reg r = newReg(T(e->type));
    emitArgs(Op::Closure, r, target, {});
    return r;
}

Reg Lowerer::lowerClosure(LambdaExpr* e) {
    std::vector<Reg> captured;
    for (sema::Local* l : e->captures) {
        captured.push_back(l->reg);
    }
    std::uint32_t index = allocFunction();
    lambdaWork_.push_back({e, subst_, index, fn_->name + ".<lambda>"});
    Reg r = newReg(T(e->type));
    emitArgs(Op::Closure, r, index, captured);
    return r;
}

Reg Lowerer::lowerIndirectCall(CallExpr* e, Reg dst) {
    Reg callee = lowerExpr(e->callee);
    const TypeInfo& ft = types_->info(T(e->callee->type));
    std::vector<TypeId> params = ft.elems;
    std::vector<Reg> args;
    for (std::size_t i = 0; i < e->ordered.size(); ++i) {
        args.push_back(lowerExpr(e->ordered[i], i < params.size() ? params[i] : kError));
    }
    Inst& in = emitArgs(Op::CallValue, dst, 0, args);
    in.a = callee;
    return dst;
}

void Lowerer::lowerLambda(const LambdaWork& w) {
    LambdaExpr* e = w.expr;
    Function& out = function(w.index);
    subst_ = w.subst;
    fn_ = &out;
    info_ = e->info;
    depth_ = 0;
    loops_.clear();
    scopes_.clear();
    out.name = w.name;
    out.result = T(e->info->result);
    openLocals_.clear();
    for (Param& p : e->params) {
        p.local->reg = newReg(T(p.local->type));
        p.local->scopeDepth = 0;
        nameLocal(p.local);
    }
    // Captures come last, so calling a lambda and calling a named function look the same to the caller.
    for (sema::Local* l : e->captures) {
        l->reg = newReg(T(l->type));
        l->scopeDepth = 0;
        nameLocal(l);
    }
    out.paramCount = static_cast<std::uint32_t>(out.regs.size());
    retReg_ = out.result != kVoid ? newReg(out.result) : kNoReg;
    setLine(e->span);
    Reg v = kNoReg;
    if (e->body->kind == ExprKind::Block && out.result == kVoid) {
        lowerBlockStmts(static_cast<BlockExpr*>(e->body));
    } else if (e->body->kind == ExprKind::Block) {
        v = lowerBlockValue(static_cast<BlockExpr*>(e->body), out.result);
    } else if (out.result != kVoid) {
        v = lowerExpr(e->body, out.result);
    } else if (e->body->kind == ExprKind::If) {
        lowerIf(static_cast<IfExpr*>(e->body), kError, false);
    } else if (e->body->kind == ExprKind::Match) {
        lowerMatch(static_cast<MatchExpr*>(e->body), kError, false);
    } else {
        lowerExpr(e->body);
    }
    TypeId bodyType = T(e->body->type);
    if (out.result == kVoid) {
        emit(Op::Return);
    } else if (v != kNoReg && bodyType != kNever && bodyType != kVoid) {
        emit(Op::Return, kNoReg, v);
    } else {
        emit(Op::Trap, kNoReg, kNoReg, kNoReg, static_cast<std::uint32_t>(TrapCode::Unreachable));
    }
    closeLocals(0);
    subst_.clear();
}

} // namespace quartz::ir
