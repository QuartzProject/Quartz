#include "sema/SemaImpl.hpp"

#include <format>

namespace quartz::sema {

// `for` binds list elements (index first), map keys or keys and values, chars (byte offset first) or items.
TypeId Checker::iterationItem(ForStmt* s, TypeId it, TypeId& indexType) {
    auto direct = [&](TypeId t) -> TypeId {
        const ir::TypeInfo& info = types_->info(t);
        if (info.kind == ir::TypeKind::List) {
            return info.elem;
        }
        if (info.array) {
            return info.elems[0];
        }
        if (info.kind == ir::TypeKind::Map) {
            if (!s->indexName.valid()) {
                return info.key;
            }
            indexType = info.key;
            return info.elem;
        }
        if (t == ir::kStr) {
            return ir::kChar;
        }
        if (implements(t, langTrait(LangTrait::Iterator)) && types_->kind(t) != ir::TypeKind::Param) {
            if (FnInfo* next = methodNamed(t, "next")) {
                ir::TypeSubst subst;
                for (std::size_t i = 0; i < next->typeParams.size() && i < info.typeArgs.size(); ++i) {
                    subst.emplace_back(next->typeParams[i], info.typeArgs[i]);
                }
                TypeId item = iteratorItem(next->owner, next);
                return item == ir::kError ? ir::kVoid : types_->subst(item, subst);
            }
        }
        return ir::kError;
    };
    TypeId item = direct(it);
    FnInfo* iter = item == ir::kError ? methodNamed(it, "iter") : nullptr;
    if (iter != nullptr && iter->hasSelf && iter->params.size() == 1 && !iter->isSuspend) {
        // `for x in c` over a type with `fn iter(self)` loops over `c.iter()`, as Iterable does in Java.
        auto* member = arena_.make<MemberExpr>();
        member->kind = ExprKind::Member;
        member->span = s->iterable->span;
        member->object = s->iterable;
        member->name = names_.intern("iter");
        auto* call = arena_.make<CallExpr>();
        call->kind = ExprKind::Call;
        call->span = s->iterable->span;
        call->callee = member;
        call->type = finishCall(call, iter, s->iterable);
        s->iterable = call;
        if (call->type == ir::kError) {
            return ir::kError;
        }
        it = call->type;
        item = direct(it);
    }
    if (item == ir::kVoid) {
        return ir::kError;
    }
    if (item == ir::kError) {
        diags_.error(s->iterable->span, std::format("cannot iterate over '{}'; lists, maps, str, types that implement "
                                                    "Iterator and types with 'fn iter(self)' returning one can be",
                                                    typeName(it)));
    }
    return item;
}

FnInfo* Checker::methodNamed(TypeId t, std::string_view name) {
    const ir::TypeInfo& info = types_->info(t);
    if (info.kind != ir::TypeKind::Struct && info.kind != ir::TypeKind::Enum) {
        return nullptr;
    }
    TypeId key = info.genericBase != 0 ? info.genericBase : t;
    auto& table = info.genericBase != 0 ? genericMethods_ : methods_;
    auto m = table.find({key, names_.intern(name).id});
    return m != table.end() ? m->second : nullptr;
}

// `m.keys()` and `m.values()`: new lists in insertion order.
TypeId Checker::mapListCall(CallExpr* call, MemberExpr* callee, TypeId recv) {
    std::string_view name = str(callee->name);
    if (types_->kind(recv) != ir::TypeKind::Map || (name != "keys" && name != "values")) {
        return ir::kError;
    }
    call->callKind = CallKind::Special;
    call->receiver = callee->object;
    call->builtin = static_cast<std::uint16_t>(name == "keys" ? SpecialOp::MapKeys : SpecialOp::MapValues);
    bindArgs(call, {}, {}, {}, 0);
    const ir::TypeInfo& info = types_->info(recv);
    return types_->list(name == "keys" ? info.key : info.elem);
}

} // namespace quartz::sema
