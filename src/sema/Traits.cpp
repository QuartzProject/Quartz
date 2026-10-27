#include "sema/SemaImpl.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace quartz::sema {

namespace {

struct LangTraitSpec {
    LangTrait lang;
    std::string_view name;
    std::string_view method; // empty for marker traits
    TypeId result;
    bool takesOther;
};

constexpr LangTraitSpec kLangTraits[] = {
    {LangTrait::Eq, "Eq", "eq", ir::kBool, true},
    {LangTrait::Hash, "Hash", "", ir::kVoid, false},
    {LangTrait::Ord, "Ord", "cmp", ir::kI64, true},
    {LangTrait::Show, "Show", "show", ir::kStr, false},
    {LangTrait::Iterator, "Iterator", "", ir::kVoid, false},
    {LangTrait::Decode, "Decode", "", ir::kVoid, false},
    {LangTrait::Encode, "Encode", "", ir::kVoid, false},
};

} // namespace

void Checker::declareLangTraits() {
    for (const LangTraitSpec& spec : kLangTraits) {
        auto* info = prog_->traitStorage.emplace_back(std::make_unique<TraitInfo>()).get();
        info->name = std::string(spec.name);
        info->lang = spec.lang;
        info->self = types_->param("Self");
        langTraits_[static_cast<std::size_t>(spec.lang)] = info;
        if (spec.method.empty()) {
            continue;
        }
        auto* decl = arena_.make<FnDecl>();
        decl->kind = DeclKind::Fn;
        decl->name = names_.intern(spec.method);
        decl->hasSelf = true;
        auto* fn = prog_->fnStorage.emplace_back(std::make_unique<FnInfo>()).get();
        fn->decl = decl;
        fn->trait = info;
        fn->owner = info->self;
        fn->hasSelf = true;
        fn->name = std::format("{}.{}", spec.name, spec.method);
        fn->params.push_back(info->self);
        fn->paramNames.push_back(names_.intern("self"));
        fn->defaults.push_back(nullptr);
        if (spec.takesOther) {
            fn->params.push_back(info->self);
            fn->paramNames.push_back(names_.intern("other"));
            fn->defaults.push_back(nullptr);
        }
        fn->result = spec.result;
        info->methods.push_back(fn);
    }
}

TraitInfo* Checker::langTraitNamed(std::string_view name) const {
    for (const LangTraitSpec& spec : kLangTraits) {
        if (spec.name == name) {
            return langTraits_[static_cast<std::size_t>(spec.lang)];
        }
    }
    return nullptr;
}

// `5.show()`, `"a".cmp("b")`: a language trait method on a type that implements it without an impl block.
TypeId Checker::langMethodCall(CallExpr* call, MemberExpr* callee, TypeId recv) {
    for (TraitInfo* t : langTraits_) {
        for (FnInfo* m : t != nullptr ? t->methods : std::vector<FnInfo*>{}) {
            if (m->decl->name != callee->name || !builtinImplements(recv, t->lang)) {
                continue;
            }
            std::vector<TypeId> params(m->params.size() - 1, recv);
            std::vector<Symbol> names(m->paramNames.begin() + 1, m->paramNames.end());
            bindArgs(call, params, names, std::vector<Expr*>(params.size(), nullptr), 0);
            call->fn = m;
            call->receiver = callee->object;
            call->traitCall = true;
            call->callKind = CallKind::Function;
            return m->result;
        }
    }
    return ir::kError;
}

TraitInfo* Checker::findTrait(Symbol n) {
    if (TraitInfo* t = langTraitNamed(str(n))) {
        return t;
    }
    return static_cast<TraitInfo*>(findName(n, NameKind::Trait));
}

void Checker::useDefaultMethod(TypeId owner, FnInfo* method) {
    const ir::TypeInfo& oinfo = types_->info(owner);
    bool genericOwner = oinfo.genericBase != 0 && types_->hasParams(owner);
    TypeId key0 = genericOwner ? oinfo.genericBase : owner;
    auto& table = genericOwner ? genericMethods_ : methods_;
    auto key = std::make_pair(key0, method->decl->name.id);
    if (!table.contains(key)) {
        table[key] = method;
        prog_->methodTable[{key0, std::string(str(method->decl->name))}] = method;
    }
}

bool Checker::builtinImplements(TypeId t, LangTrait lang) {
    const ir::TypeInfo& info = types_->info(t);
    auto all = [&](const std::vector<TypeId>& ts) {
        return std::all_of(ts.begin(), ts.end(),
                           [&](TypeId e) { return implements(e, langTraits_[std::size_t(lang)]); });
    };
    bool hashable =
        types_->isInteger(t) || t == ir::kBool || t == ir::kChar || t == ir::kStr || info.kind == ir::TypeKind::Handle;
    switch (lang) {
    case LangTrait::Eq:
        if (info.kind == ir::TypeKind::Enum && info.genericBase == 0 && !types_->isResult(t)) {
            if (std::all_of(info.variants.begin(), info.variants.end(),
                            [](const auto& v) { return v.payload.empty(); })) {
                return true; // payload-free enums compare their tags, as before traits existed
            }
        }
        if (info.kind == ir::TypeKind::Nullable || info.kind == ir::TypeKind::List) {
            return implements(info.elem, langTraits_[std::size_t(lang)]);
        }
        return hashable || types_->isFloat(t) || (info.kind == ir::TypeKind::Tuple && all(info.elems));
    case LangTrait::Hash:
        if (info.kind == ir::TypeKind::List) {
            return implements(info.elem, langTraits_[std::size_t(lang)]); // keys compare lists by content (D133)
        }
        return hashable || (info.kind == ir::TypeKind::Tuple && all(info.elems));
    case LangTrait::Ord:
        if (info.kind == ir::TypeKind::List) {
            return implements(info.elem, langTraits_[std::size_t(lang)]);
        }
        return types_->isNumeric(t) || t == ir::kChar || t == ir::kStr ||
               (info.kind == ir::TypeKind::Tuple && all(info.elems));
    case LangTrait::Show:
        if (info.kind == ir::TypeKind::Nullable || info.kind == ir::TypeKind::List) {
            return implements(info.elem, langTraits_[std::size_t(lang)]);
        }
        if (info.kind == ir::TypeKind::Map) {
            return implements(info.key, langTraits_[std::size_t(lang)]) &&
                   implements(info.elem, langTraits_[std::size_t(lang)]);
        }
        return types_->isNumeric(t) || t == ir::kBool || t == ir::kChar || t == ir::kStr ||
               (info.kind == ir::TypeKind::Tuple && all(info.elems));
    case LangTrait::Decode:
    case LangTrait::Encode:
        return codecBuiltin(t, lang);
    default:
        return false;
    }
}

bool Checker::implements(TypeId type, TraitInfo* trait) {
    if (type == ir::kError) {
        return true;
    }
    if (types_->kind(type) == ir::TypeKind::Param) {
        auto it = paramBounds_.find(type);
        if (it == paramBounds_.end()) {
            return false;
        }
        auto has = [&](TraitInfo* t) { return std::find(it->second.begin(), it->second.end(), t) != it->second.end(); };
        // Ord and Hash both need equality, so either bound also allows `==`.
        return has(trait) ||
               (trait->lang == LangTrait::Eq && (has(langTrait(LangTrait::Ord)) || has(langTrait(LangTrait::Hash))));
    }
    if (trait->lang != LangTrait::None && builtinImplements(type, trait->lang)) {
        return true;
    }
    const ir::TypeInfo& info = types_->info(type);
    TypeId key = info.genericBase != 0 ? info.genericBase : type;
    if (!traitImpls_.contains({trait, key})) {
        return false;
    }
    auto bounds = implBounds_.find({trait, key});
    if (info.genericBase == 0 || bounds == implBounds_.end()) {
        return true;
    }
    for (std::size_t k = 0; k < bounds->second.size() && k < info.typeArgs.size(); ++k) {
        for (TraitInfo* b : bounds->second[k]) {
            if (!implements(info.typeArgs[k], b)) {
                return false;
            }
        }
    }
    return true;
}

void Checker::resolveTypeBounds() {
    for (auto& [base, params] : typeParamDecls_) {
        currentFile_ = params.second;
        std::vector<std::vector<TraitInfo*>> all;
        const std::vector<TypeId>& ids = types_->info(base).typeParams;
        for (std::size_t k = 0; k < params.first.size(); ++k) {
            std::vector<TraitInfo*> bs;
            for (Symbol b : params.first[k].bounds) {
                if (TraitInfo* t = findTrait(b)) {
                    bs.push_back(t);
                } else {
                    diags_.error(params.first[k].span, std::format("unknown trait '{}'", str(b)));
                }
            }
            if (k < ids.size()) {
                paramBounds_[ids[k]] = bs;
            }
            all.push_back(std::move(bs));
        }
        typeBounds_[base] = std::move(all);
    }
}

void Checker::mergeTypeBounds(TypeId owner, ImplParams& ip) {
    const ir::TypeInfo& info = types_->info(owner);
    auto it = typeBounds_.find(info.genericBase);
    if (info.genericBase == 0 || it == typeBounds_.end()) {
        return;
    }
    for (std::size_t k = 0; k < it->second.size() && k < ip.ids.size(); ++k) {
        for (TraitInfo* t : it->second[k]) {
            std::vector<TraitInfo*>& have = paramBounds_[ip.ids[k]];
            if (std::find(have.begin(), have.end(), t) == have.end()) {
                have.push_back(t);
                ip.bounds[k].push_back(t);
            }
        }
    }
}

void Checker::checkTypeArgs(TypeId instance, Span span) {
    if (!boundsReady_) {
        pendingInstances_.emplace_back(instance, span);
        return;
    }
    const ir::TypeInfo& info = types_->info(instance);
    auto it = typeBounds_.find(info.genericBase);
    if (info.genericBase == 0 || it == typeBounds_.end()) {
        return;
    }
    for (std::size_t k = 0; k < it->second.size() && k < info.typeArgs.size(); ++k) {
        for (TraitInfo* t : it->second[k]) {
            if (!implements(info.typeArgs[k], t)) {
                diags_.error(span,
                             std::format("'{}' does not implement '{}', which '{}' requires of its parameter",
                                         typeName(info.typeArgs[k]), t->name, types_->info(info.genericBase).name));
            }
        }
    }
}

void Checker::checkMapKey(TypeId key, Span span) {
    if (!boundsReady_) {
        pendingKeys_.emplace_back(key, span);
        return;
    }
    if (!implements(key, langTrait(LangTrait::Hash))) {
        diags_.error(span, std::format("map keys must implement Hash, and '{}' does not; integers, bool, char, str, "
                                       "handles, tuples of them and types with @derive(Eq, Hash) do",
                                       typeName(key)));
    }
}

void Checker::flushPendingChecks() {
    boundsReady_ = true;
    for (auto& [t, span] : std::exchange(pendingInstances_, {})) {
        currentFile_ = span.file;
        checkTypeArgs(t, span);
    }
    for (auto& [t, span] : std::exchange(pendingKeys_, {})) {
        currentFile_ = span.file;
        checkMapKey(t, span);
    }
}

} // namespace quartz::sema
