#include "sema/SemaImpl.hpp"

#include <algorithm>
#include <format>

namespace quartz::sema {

namespace {

std::string_view derivedMethod(LangTrait lang) {
    switch (lang) {
    case LangTrait::Eq:
        return "eq";
    case LangTrait::Ord:
        return "cmp";
    case LangTrait::Show:
        return "show";
    default:
        return "";
    }
}

} // namespace

void Checker::declareDerives() {
    for (auto& entryPtr : typeEntries_) {
        Decl* d = entryPtr->enumDecl != nullptr ? static_cast<Decl*>(entryPtr->enumDecl) : entryPtr->decl;
        currentFile_ = d->span.file;
        for (const Attribute& a : d->attrs) {
            if (str(a.name) != "derive") {
                continue;
            }
            ir::TypeKind kind = types_->kind(entryPtr->type);
            if (kind != ir::TypeKind::Struct && kind != ir::TypeKind::Enum) {
                diags_.error(a.span, "@derive goes on a struct or an enum");
                continue;
            }
            for (const AttrArg& arg : a.args) {
                TraitInfo* t = arg.value->kind == ExprKind::Ident && !arg.name.valid()
                                   ? langTraitNamed(str(static_cast<IdentExpr*>(arg.value)->name))
                                   : nullptr;
                if (t == nullptr || t->lang == LangTrait::Iterator) {
                    diags_.error(arg.value->span, "@derive takes Eq, Hash, Ord, Show, Decode and Encode");
                    continue;
                }
                bool codec = t->lang == LangTrait::Decode || t->lang == LangTrait::Encode;
                if (codec && !codecTypes(arg.value->span)) {
                    continue;
                }
                if (!traitImpls_.insert({t, entryPtr->type}).second) {
                    diags_.error(arg.value->span, std::format("'{}' is derived twice", t->name));
                    continue;
                }
                derives_.push_back({entryPtr->type, t, arg.value->span});
                deriveTrait(entryPtr->type, t);
            }
        }
    }
}

void Checker::deriveTrait(TypeId type, TraitInfo* trait) {
    const ir::TypeInfo info = types_->info(type); // a copy: instantiate below may grow the table
    std::vector<std::vector<TraitInfo*>> bounds;
    auto tb = typeBounds_.find(type);
    for (std::size_t k = 0; k < info.typeParams.size(); ++k) {
        std::vector<TraitInfo*> b =
            tb != typeBounds_.end() && k < tb->second.size() ? tb->second[k] : std::vector<TraitInfo*>{};
        b.push_back(trait);
        bounds.push_back(std::move(b));
    }
    if (!info.typeParams.empty()) {
        implBounds_[{trait, type}] = bounds;
    }
    std::string_view method = derivedMethod(trait->lang);
    bool codec = trait->lang == LangTrait::Decode || trait->lang == LangTrait::Encode;
    if (method.empty() && !codec) {
        return;
    }
    TypeId owner = info.typeParams.empty() ? type : types_->instantiate(type, info.typeParams);
    auto* fn =
        codec ? deriveCodec(type, owner, trait) : prog_->fnStorage.emplace_back(std::make_unique<FnInfo>()).get();
    fn->derived = trait->lang;
    fn->owner = owner;
    fn->typeParams = info.typeParams;
    for (auto& [sym, pid] : baseParams_[type]) {
        fn->typeParamNames.push_back(sym);
    }
    fn->bounds = std::move(bounds);
    if (codec) {
        if (fn->generic()) {
            prog_->generics.push_back(fn);
        } else {
            fn->index = static_cast<std::uint32_t>(prog_->functions.size());
            prog_->functions.push_back(fn);
        }
        return;
    }
    fn->hasSelf = true;
    fn->name = std::format("{}.{}", info.name, method);
    fn->params.push_back(owner);
    fn->paramNames.push_back(names_.intern("self"));
    fn->defaults.push_back(nullptr);
    if (trait->lang != LangTrait::Show) {
        fn->params.push_back(owner);
        fn->paramNames.push_back(names_.intern("other"));
        fn->defaults.push_back(nullptr);
    }
    fn->result = trait->methods[0]->result;
    if (fn->generic()) {
        prog_->generics.push_back(fn);
    } else {
        fn->index = static_cast<std::uint32_t>(prog_->functions.size());
        prog_->functions.push_back(fn);
    }
    auto key = std::make_pair(type, names_.intern(method).id);
    auto& table = fn->generic() ? genericMethods_ : methods_;
    table[key] = fn;
    prog_->methodTable[{type, std::string(method)}] = fn;
}

void Checker::checkDerives() {
    for (const DeriveInfo& d : derives_) {
        currentFile_ = d.span.file;
        const ir::TypeInfo& info = types_->info(d.type);
        // Inside the type its parameters stand for any type with the derived trait.
        std::vector<std::pair<TypeId, std::vector<TraitInfo*>>> saved;
        for (TypeId p : info.typeParams) {
            saved.emplace_back(p, paramBounds_[p]);
            paramBounds_[p].push_back(d.trait);
        }
        auto need = [&](TypeId ft, const std::string& what) {
            if (!implements(ft, d.trait)) {
                diags_.error(d.span, std::format("cannot derive '{}' for '{}': {} of type '{}' does not implement it",
                                                 d.trait->name, info.name, what, typeName(ft)));
            }
        };
        for (const ir::Field& f : info.fields) {
            need(f.type, std::format("field '{}'", f.name));
        }
        for (const ir::VariantInfo& v : info.variants) {
            for (TypeId p : v.payload) {
                need(p, std::format("variant '{}'", v.name));
            }
        }
        if (d.trait->lang == LangTrait::Hash && info.kind == ir::TypeKind::Enum) {
            std::vector<std::vector<bool>> boxed = types_->recursivePayloads(d.type);
            if (std::any_of(boxed.begin(), boxed.end(), [](const auto& v) { return std::ranges::contains(v, true); })) {
                diags_.error(d.span, std::format("cannot derive 'Hash' for '{}': a recursive enum cannot be a map key",
                                                 info.name));
            }
        }
        for (auto& [p, b] : saved) {
            paramBounds_[p] = std::move(b);
        }
        auto derivedToo = [&](LangTrait lang) {
            return std::any_of(derives_.begin(), derives_.end(),
                               [&](const DeriveInfo& o) { return o.type == d.type && o.trait->lang == lang; });
        };
        if (d.trait->lang == LangTrait::Decode ||
            (d.trait->lang == LangTrait::Encode && !derivedToo(LangTrait::Decode))) {
            if (codecTypes(d.span)) {
                checkCodecKeys(d);
            }
        }
        if (d.trait->lang == LangTrait::Hash && !derivedToo(LangTrait::Eq)) {
            diags_.error(d.span, "deriving Hash needs Eq derived too: maps compare keys field by field");
        }
        if (d.trait->lang == LangTrait::Ord && !traitImpls_.contains({langTrait(LangTrait::Eq), d.type})) {
            diags_.error(d.span, "Ord needs Eq: derive or implement it too");
        }
    }
    for (auto& [t, span] : manualOrd_) {
        currentFile_ = span.file;
        const ir::TypeInfo& info = types_->info(t);
        if (!traitImpls_.contains({langTrait(LangTrait::Eq), info.genericBase != 0 ? info.genericBase : t})) {
            diags_.error(span, "Ord needs Eq: derive or implement it too");
        }
    }
}

bool Checker::checkLangImpl(ImplDecl* impl, TypeId owner, TraitInfo* trait, const std::vector<FnInfo*>& methods) {
    if (builtinImplements(owner, trait->lang)) {
        diags_.error(impl->span, std::format("'{}' already implements '{}'", typeName(owner), trait->name));
        return false;
    }
    switch (trait->lang) {
    case LangTrait::Hash:
        diags_.error(impl->span, "Hash can only be derived: maps hash and compare keys field by field");
        return false;
    case LangTrait::Decode:
    case LangTrait::Encode:
        diags_.error(impl->span, std::format("{} can only be derived", trait->name));
        return false;
    case LangTrait::Ord:
        manualOrd_.emplace_back(owner, impl->span);
        return true;
    case LangTrait::Iterator: {
        FnInfo* next = nullptr;
        for (FnInfo* m : methods) {
            next = str(m->decl->name) == "next" ? m : next;
        }
        TypeId item = next != nullptr ? iteratorItem(owner, next) : ir::kError;
        if (item == ir::kError) {
            diags_.error(next != nullptr ? next->decl->span : impl->span,
                         std::format("Iterator needs 'fn next(self) -> (T, {})?': the next item and the advanced "
                                     "iterator, or null at the end",
                                     typeName(owner)));
        }
        return true;
    }
    default:
        return true;
    }
}

TypeId Checker::iteratorItem(TypeId owner, FnInfo* next) {
    if (next->params.size() != 1 || types_->kind(next->result) != ir::TypeKind::Nullable) {
        return ir::kError;
    }
    const ir::TypeInfo& pair = types_->info(types_->info(next->result).elem);
    if (pair.kind != ir::TypeKind::Tuple || pair.elems.size() != 2 || pair.elems[1] != owner) {
        return ir::kError;
    }
    return pair.elems[0];
}

} // namespace quartz::sema
