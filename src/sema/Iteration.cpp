#include "sema/SemaImpl.hpp"

#include <format>

namespace quartz::sema {

// What `for` binds over each iterable: lists give elements (the index first), maps their keys or key and value in
// insertion order, `str` its chars (the byte offset first) and an Iterator its items (a count first).
TypeId Checker::iterationItem(ForStmt* s, TypeId it, TypeId& indexType) {
    const ir::TypeInfo& info = types_->info(it);
    if (info.kind == ir::TypeKind::List) {
        return info.elem;
    }
    if (info.kind == ir::TypeKind::Map) {
        if (!s->indexName.valid()) {
            return info.key;
        }
        indexType = info.key;
        return info.elem;
    }
    if (it == ir::kStr) {
        return ir::kChar;
    }
    if (implements(it, langTrait(LangTrait::Iterator)) && types_->kind(it) != ir::TypeKind::Param) {
        TypeId key = info.genericBase != 0 ? info.genericBase : it;
        auto& table = info.genericBase != 0 ? genericMethods_ : methods_;
        auto m = table.find({key, names_.intern("next").id});
        if (m != table.end()) {
            ir::TypeSubst subst;
            for (std::size_t i = 0; i < m->second->typeParams.size() && i < info.typeArgs.size(); ++i) {
                subst.emplace_back(m->second->typeParams[i], info.typeArgs[i]);
            }
            TypeId item = iteratorItem(m->second->owner, m->second);
            return item == ir::kError ? item : types_->subst(item, subst);
        }
    }
    diags_.error(s->iterable->span, std::format("cannot iterate over '{}'; lists, maps, str and types that implement "
                                                "Iterator can be",
                                                typeName(it)));
    return ir::kError;
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
