#include "sema/SemaImpl.hpp"

#include <format>

namespace quartz::sema {

// `[T; N]`: N is any constant integer expression, evaluated now (D144).
TypeId Checker::arrayType(ArrayTypeExpr* t) {
    TypeId elem = resolveType(t->elem);
    TypeId st = checkValue(t->size, ir::kI64);
    if (st != ir::kError && !types_->isInteger(st)) {
        diags_.error(t->size->span, std::format("an array length must be an integer, found '{}'", typeName(st)));
        return ir::kError;
    }
    std::optional<ConstValue> n = st == ir::kError ? std::nullopt : evalConst(t->size);
    if (!n || elem == ir::kError) {
        return ir::kError;
    }
    bool negative = types_->isSigned(st) && static_cast<std::int64_t>(n->bits) < 0;
    if (negative || n->bits == 0 || n->bits > ir::kMaxArrayLength) {
        diags_.error(t->size->span, std::format("an array length must be between 1 and {}", ir::kMaxArrayLength));
        return ir::kError;
    }
    ir::TypeKind k = types_->kind(elem);
    if (elem == ir::kVoid || k == ir::TypeKind::State) {
        diags_.error(t->elem->span, std::format("an array cannot hold '{}'", typeName(elem)));
        return ir::kError;
    }
    return types_->array(elem, static_cast<std::uint32_t>(n->bits));
}

TypeId Checker::checkArrayLiteral(Expr* e, TypeId want) {
    const ir::TypeInfo& info = types_->info(want);
    TypeId elem = info.elems[0];
    auto n = static_cast<std::uint64_t>(info.elems.size());
    std::uint64_t got = 0;
    if (e->kind == ExprKind::ListLit) {
        auto* l = static_cast<ListLitExpr*>(e);
        for (Expr* x : l->elems) {
            expectAssignable(checkValue(x, elem), elem, x->span);
        }
        got = l->elems.size();
    } else {
        auto* r = static_cast<ListRepeatExpr*>(e);
        expectAssignable(checkValue(r->value, elem), elem, r->value->span);
        TypeId ct = checkValue(r->count, ir::kI64);
        if (ct == ir::kError || !types_->isInteger(ct)) {
            diags_.error(r->count->span, "the array length must be an integer");
            return want;
        }
        std::optional<ConstValue> c = evalConst(r->count);
        if (!c) {
            return want;
        }
        got = c->bits;
    }
    if (got != n) {
        diags_.error(e->span, std::format("this has {} elements; '{}' needs {}", got, typeName(want), n));
    }
    return want;
}

TypeId Checker::checkArrayIndex(IndexExpr* e, TypeId array) {
    TypeId it = checkValue(e->index, ir::kI64);
    if (it != ir::kError && !types_->isInteger(it)) {
        diags_.error(e->index->span, std::format("array index must be an integer, found '{}'", typeName(it)));
    }
    return types_->info(array).elems[0];
}

// `len()` is the constant length and `toList()` copies the elements into a new list.
TypeId Checker::arrayMethod(CallExpr* call, MemberExpr* callee, TypeId recv) {
    std::string_view name = str(callee->name);
    const ir::TypeInfo& info = types_->info(recv);
    if (name != "len" && name != "toList") {
        diags_.error(callee->span, std::format("'{}' has no method '{}'", typeName(recv), name));
        for (Arg& a : call->args) {
            check(a.value, ir::kError);
        }
        return ir::kError;
    }
    call->callKind = CallKind::Special;
    call->receiver = callee->object;
    call->builtin = static_cast<std::uint16_t>(name == "len" ? SpecialOp::ArrayLen : SpecialOp::ArrayToList);
    bindArgs(call, {}, {}, {}, 0);
    return name == "len" ? ir::kI64 : types_->list(info.elems[0]);
}

} // namespace quartz::sema
