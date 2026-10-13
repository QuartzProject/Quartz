#include "sema/SemaImpl.hpp"

#include <format>

namespace quartz::sema {

// The calls whose value is the branch's value: the body, or the last expression of a block.
void Checker::markHoleSites(Expr* body) {
    if (body == nullptr) {
        return;
    }
    switch (body->kind) {
    case ExprKind::Call:
        if (!holes_.empty()) {
            holeSites_.insert(body);
            holes_.back().sites.push_back(body);
        }
        return;
    case ExprKind::Block: {
        auto* b = static_cast<BlockExpr*>(body);
        if (!b->stmts.empty() && b->stmts.back()->kind == StmtKind::Expr) {
            markHoleSites(static_cast<ExprStmt*>(b->stmts.back())->expr);
        }
        return;
    }
    default:
        return;
    }
}

bool Checker::resultHole(CallExpr* call, std::uint32_t variant) {
    if (holes_.empty() || !holeSites_.contains(call) || call->args.size() != 1 || call->args[0].name.valid()) {
        return false;
    }
    TypeId arg = checkValue(call->args[0].value, ir::kError);
    call->callKind = CallKind::Variant;
    call->variant = variant;
    std::vector<Expr*> ordered{call->args[0].value};
    call->ordered = arena_.copy(ordered);
    holes_.back().holes.push_back({call, variant, arg});
    return true;
}

namespace {

// A block or `else if` that only looked diverging because its value was a hole takes the filled type.
void retype(Expr* e, TypeId t) {
    if (e == nullptr || e->type != ir::kNever) {
        return;
    }
    switch (e->kind) {
    case ExprKind::Block: {
        auto* b = static_cast<BlockExpr*>(e);
        if (!b->stmts.empty() && b->stmts.back()->kind == StmtKind::Expr) {
            Expr* last = static_cast<ExprStmt*>(b->stmts.back())->expr;
            retype(last, t);
            if (last->type != ir::kNever) {
                b->type = t;
            }
        }
        return;
    }
    case ExprKind::If: {
        auto* i = static_cast<IfExpr*>(e);
        retype(i->then, t);
        retype(i->otherwise, t);
        bool value = i->then->type != ir::kNever || (i->otherwise != nullptr && i->otherwise->type != ir::kNever);
        if (value) {
            i->type = t;
        }
        return;
    }
    default:
        return;
    }
}

} // namespace

TypeId Checker::fillResultHoles(TypeId unified, const std::vector<Expr*>& bodies) {
    std::vector<ResultHole> holes = std::move(holes_.back().holes);
    for (const Expr* site : holes_.back().sites) {
        holeSites_.erase(site);
    }
    holes_.pop_back();
    if (holes.empty()) {
        return unified;
    }
    TypeId target = unified;
    if (target == ir::kNever || target == ir::kNull) {
        TypeId sides[2] = {ir::kNever, ir::kNever};
        for (const ResultHole& h : holes) {
            sides[h.variant] = unify(sides[h.variant], h.arg, h.call->span);
        }
        for (const ResultHole& h : holes) {
            if (sides[1 - h.variant] == ir::kNever) {
                diags_.error(h.call->span,
                             std::format("cannot infer the {} type of '{}(...)': no other branch gives it; "
                                         "annotate the type",
                                         h.variant == 0 ? "error" : "value", h.variant == 0 ? "Ok" : "Err"));
                target = ir::kError;
            }
        }
        if (target != ir::kError && sides[0] != ir::kError && sides[1] != ir::kError) {
            TypeId r = types_->result(sides[0], sides[1]);
            target = unified == ir::kNull ? types_->nullable(r) : r;
        } else {
            target = ir::kError;
        }
    }
    TypeId want = target;
    if (want != ir::kError && types_->kind(want) == ir::TypeKind::Nullable) {
        want = types_->info(want).elem;
    }
    if (want != ir::kError && !types_->isResult(want)) {
        diags_.error(holes[0].call->span, std::format("'{}(...)' is a Result, but another branch is a '{}'",
                                                      holes[0].variant == 0 ? "Ok" : "Err", typeName(target)));
        target = ir::kError;
    }
    for (const ResultHole& h : holes) {
        if (target == ir::kError) {
            h.call->type = ir::kError;
            continue;
        }
        TypeId payload = types_->info(want).variants[h.variant].payload[0];
        expectAssignable(h.arg, payload, h.call->args[0].value->span);
        h.call->type = want;
    }
    for (Expr* body : bodies) {
        retype(body, target == ir::kError ? ir::kError : want);
    }
    return target;
}

} // namespace quartz::sema
