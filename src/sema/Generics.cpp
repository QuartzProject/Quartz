#include "sema/SemaImpl.hpp"

#include <algorithm>
#include <format>

namespace quartz::sema {

void Checker::pushTypeParams(std::span<TypeParam> params, std::vector<TypeId>& ids, std::vector<Symbol>& names,
                             std::vector<std::vector<TraitInfo*>>& bounds) {
    typeScopes_.emplace_back();
    for (const TypeParam& p : params) {
        TypeId id = types_->param(std::string(str(p.name)));
        typeScopes_.back()[p.name] = id;
        ids.push_back(id);
        names.push_back(p.name);
        std::vector<TraitInfo*> bs;
        for (Symbol b : p.bounds) {
            if (TraitInfo* t = findTrait(b)) {
                bs.push_back(t);
            } else {
                diags_.error(p.span, std::format("unknown trait '{}'", str(b)));
            }
        }
        paramBounds_[id] = bs;
        bounds.push_back(std::move(bs));
    }
}

void Checker::declareTraits() {
    declareLangTraits();
    for (Module* mod : modules_) {
        for (Decl* d : mod->decls) {
            if (d->kind != DeclKind::Trait) {
                continue;
            }
            auto* t = static_cast<TraitDecl*>(d);
            if (langTraitNamed(str(t->name)) != nullptr) {
                diags_.error(t->nameSpan,
                             std::format("'{}' is a language trait; implement it or derive it instead", str(t->name)));
                continue;
            }
            auto* info = prog_->traitStorage.emplace_back(std::make_unique<TraitInfo>()).get();
            info->name = std::string(str(t->name));
            info->self = types_->param("Self");
            addName(t->name, NameKind::Trait, info, t, mod->file->id());
            traitList_.emplace_back(t, info);
        }
    }
    for (auto& [t, info] : traitList_) {
        currentFile_ = t->span.file;
        {
            for (FnDecl* m : t->methods) {
                if (!m->hasSelf || !m->typeParams.empty()) {
                    diags_.error(m->span, "trait methods take 'self' and no type parameters");
                }
                auto* fn = prog_->fnStorage.emplace_back(std::make_unique<FnInfo>()).get();
                fn->decl = m;
                fn->trait = info;
                fn->owner = info->self;
                fn->hasSelf = true;
                fn->name = std::format("{}.{}", info->name, str(m->name));
                TypeId savedSelf = selfType_;
                selfType_ = info->self;
                fn->params.push_back(info->self);
                fn->paramNames.push_back(names_.intern("self"));
                fn->defaults.push_back(nullptr);
                for (Param& p : m->params) {
                    fn->params.push_back(resolveType(p.type));
                    fn->paramNames.push_back(p.name);
                    fn->defaults.push_back(nullptr);
                }
                fn->result = m->result != nullptr ? resolveType(m->result) : ir::kVoid;
                selfType_ = savedSelf;
                info->methods.push_back(fn);
                if (m->body != nullptr) {
                    // A default body is generic over `Self`, bounded by its own trait, and instantiated per type.
                    m->info = fn;
                    fn->typeParams = {info->self};
                    fn->typeParamNames = {names_.intern("Self")};
                    fn->bounds = {{info}};
                    paramBounds_[info->self] = {info};
                    prog_->generics.push_back(fn);
                }
            }
        }
    }
}

void Checker::declareImplTrait(ImplDecl* impl, TypeId owner, const std::vector<FnInfo*>& methods,
                               const ImplParams& ip) {
    if (impl->trait == nullptr) {
        return;
    }
    if (impl->trait->kind != TypeExprKind::Named || static_cast<NamedTypeExpr*>(impl->trait)->path.empty()) {
        diags_.error(impl->trait->span, "expected a trait name");
        return;
    }
    auto* named = static_cast<NamedTypeExpr*>(impl->trait);
    TraitInfo* trait = findTrait(named->path[named->path.size() - 1]);
    if (trait == nullptr) {
        diags_.error(impl->trait->span, "unknown trait");
        return;
    }
    const ir::TypeInfo& oinfo = types_->info(owner);
    TypeId key = oinfo.genericBase != 0 ? oinfo.genericBase : owner;
    if (trait->lang != LangTrait::None && !checkLangImpl(impl, owner, trait, methods)) {
        return;
    }
    if (!traitImpls_.insert({trait, key}).second) {
        diags_.error(impl->span, std::format("'{}' already implements '{}'", typeName(owner), trait->name));
    }
    if (!ip.ids.empty()) {
        implBounds_[{trait, key}] = ip.bounds;
    }
    for (FnInfo* want : trait->methods) {
        FnInfo* have = nullptr;
        for (FnInfo* m : methods) {
            if (m->decl->name == want->decl->name) {
                have = m;
            }
        }
        std::string mname(str(want->decl->name));
        if (have == nullptr && want->decl->body != nullptr) {
            useDefaultMethod(owner, want);
            continue;
        }
        if (have == nullptr) {
            diags_.error(impl->span, std::format("missing method '{}' of trait '{}'", mname, trait->name));
            continue;
        }
        ir::TypeSubst self{{trait->self, owner}};
        bool same = have->params.size() == want->params.size() && have->result == types_->subst(want->result, self);
        for (std::size_t i = 0; same && i < want->params.size(); ++i) {
            same = have->params[i] == types_->subst(want->params[i], self);
        }
        if (!same) {
            diags_.error(have->decl->span, std::format("method '{}' does not match its declaration in trait '{}'", mname,
                                                       trait->name));
        }
    }
}

bool Checker::unify(TypeId pattern, TypeId actual, ir::TypeSubst& bindings) {
    if (actual == ir::kError || actual == ir::kNever) {
        return true;
    }
    const ir::TypeInfo& p = types_->info(pattern);
    if (p.kind == ir::TypeKind::Param) {
        for (auto& [from, to] : bindings) {
            if (from == pattern) {
                return to == actual || (types_->kind(to) == ir::TypeKind::Nullable && assignable(actual, to));
            }
        }
        if (actual == ir::kNull) {
            return false;
        }
        bindings.emplace_back(pattern, actual);
        return true;
    }
    if (!types_->hasParams(pattern)) {
        return assignable(actual, pattern);
    }
    const ir::TypeInfo& a = types_->info(actual);
    if (p.kind == ir::TypeKind::Nullable) {
        if (actual == ir::kNull) {
            return true;
        }
        return unify(p.elem, a.kind == ir::TypeKind::Nullable ? a.elem : actual, bindings);
    }
    if (p.kind != a.kind || p.array != a.array) {
        return false;
    }
    switch (p.kind) {
    case ir::TypeKind::List: return unify(p.elem, a.elem, bindings);
    case ir::TypeKind::Map: return unify(p.key, a.key, bindings) && unify(p.elem, a.elem, bindings);
    case ir::TypeKind::Tuple:
    case ir::TypeKind::Fn: {
        if (p.elems.size() != a.elems.size()) {
            return false;
        }
        for (std::size_t i = 0; i < p.elems.size(); ++i) {
            if (!unify(p.elems[i], a.elems[i], bindings)) {
                return false;
            }
        }
        return p.kind == ir::TypeKind::Tuple || unify(p.result, a.result, bindings);
    }
    default: break;
    }
    if (types_->isResult(pattern) && types_->isResult(actual)) {
        return unify(p.variants[0].payload[0], a.variants[0].payload[0], bindings) &&
               unify(p.variants[1].payload[0], a.variants[1].payload[0], bindings);
    }
    if (p.genericBase != 0 && p.genericBase == a.genericBase && p.typeArgs.size() == a.typeArgs.size()) {
        for (std::size_t i = 0; i < p.typeArgs.size(); ++i) {
            if (!unify(p.typeArgs[i], a.typeArgs[i], bindings)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

TypeId Checker::finishGenericCall(CallExpr* call, FnInfo* fn, Expr* receiver, ir::TypeSubst bindings) {
    std::size_t skip = receiver != nullptr ? 1 : 0;
    std::size_t n = fn->params.size() - skip;
    std::vector<Expr*> ordered(n, nullptr);
    std::size_t positional = 0;
    for (Arg& a : call->args) {
        std::size_t slot = n;
        if (a.name.valid()) {
            for (std::size_t i = 0; i < n; ++i) {
                if (fn->paramNames[i + skip] == a.name) {
                    slot = i;
                }
            }
        } else {
            slot = positional++;
        }
        if (slot >= n || ordered[slot] != nullptr) {
            diags_.error(a.value->span, "unexpected argument");
            check(a.value, ir::kError);
            continue;
        }
        ordered[slot] = a.value;
    }
    // Arguments whose parameter type is already known are checked against it; the rest drive inference, lambdas
    // last so their parameter types can come from the other arguments.
    std::vector<bool> done(n, false);
    for (int pass = 0; pass < 3; ++pass) {
        for (std::size_t i = 0; i < n; ++i) {
            Expr* arg = ordered[i];
            if (arg == nullptr || done[i]) {
                continue;
            }
            TypeId pt = types_->subst(fn->params[i + skip], bindings);
            bool known = !types_->hasParams(pt);
            bool lambda = arg->kind == ExprKind::Lambda;
            if ((pass == 0 && !known) || (pass == 1 && lambda)) {
                continue;
            }
            done[i] = true;
            if (lambda && !known && types_->kind(pt) == ir::TypeKind::Fn) {
                LambdaHint hint{types_->info(pt).elems, types_->info(pt).result, {}};
                for (TypeId tp : fn->typeParams) {
                    if (std::none_of(bindings.begin(), bindings.end(), [&](const auto& b) { return b.first == tp; })) {
                        hint.unknown.push_back(tp);
                    }
                }
                lambdaHint_ = std::move(hint);
            }
            TypeId at = checkValue(arg, known ? pt : ir::kError);
            lambdaHint_.reset();
            if (!unify(fn->params[i + skip], at, bindings)) {
                diags_.error(arg->span, std::format("expected '{}', found '{}'", typeName(pt), typeName(at)));
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (ordered[i] == nullptr) {
            Expr* d = fn->defaults[i + skip];
            if (d == nullptr) {
                diags_.error(call->span, std::format("missing argument '{}'", str(fn->paramNames[i + skip])));
            } else if (d->type == ir::kError && defaultsInProgress_.insert(d).second) {
                // Checked once, in the callee's scope, like the defaults of other functions.
                auto saved = std::move(scopes_);
                scopes_.clear();
                expectAssignable(checkValue(d, fn->params[i + skip]), fn->params[i + skip], d->span);
                scopes_ = std::move(saved);
                defaultsInProgress_.erase(d);
            }
            ordered[i] = d;
        }
    }
    call->ordered = arena_.copy(ordered);
    std::vector<std::uint32_t> args;
    for (std::size_t k = 0; k < fn->typeParams.size(); ++k) {
        TypeId bound = ir::kError;
        for (auto& [from, to] : bindings) {
            if (from == fn->typeParams[k]) {
                bound = to;
            }
        }
        if (bound == ir::kError) {
            diags_.error(call->span, std::format("cannot infer type parameter '{}' of '{}'; write the types with ::<>",
                                                 str(fn->typeParamNames[k]), fn->name));
            return ir::kError;
        }
        for (TraitInfo* t : fn->bounds[k]) {
            if (!implements(bound, t)) {
                diags_.error(call->span, std::format("'{}' does not implement '{}'", typeName(bound), t->name));
            }
        }
        args.push_back(bound);
    }
    call->typeArgs = arena_.copy(args);
    call->fn = fn;
    call->receiver = receiver;
    call->callKind = fn->isHost ? CallKind::Host : CallKind::Function;
    return types_->subst(fn->result, bindings);
}

TypeId Checker::checkTraitCall(CallExpr* call, MemberExpr* callee, TypeId recv) {
    auto it = paramBounds_.find(recv);
    if (it != paramBounds_.end()) {
        std::vector<TraitInfo*> traits = it->second;
        if (implements(recv, langTrait(LangTrait::Eq))) {
            traits.push_back(langTrait(LangTrait::Eq));
        }
        for (TraitInfo* t : traits) {
            for (FnInfo* m : t->methods) {
                if (m->decl->name == callee->name) {
                    ir::TypeSubst self{{t->self, recv}};
                    std::vector<TypeId> params;
                    for (std::size_t i = 1; i < m->params.size(); ++i) {
                        params.push_back(types_->subst(m->params[i], self));
                    }
                    std::vector<Symbol> names(m->paramNames.begin() + 1, m->paramNames.end());
                    std::vector<Expr*> defaults(params.size(), nullptr);
                    bindArgs(call, params, names, defaults, 0);
                    call->fn = m;
                    call->receiver = callee->object;
                    call->traitCall = true;
                    call->callKind = CallKind::Function;
                    return types_->subst(m->result, self);
                }
            }
        }
    }
    diags_.error(callee->span, std::format("'{}' has no method '{}'; add a trait bound that declares it", typeName(recv),
                                           str(callee->name)));
    for (Arg& a : call->args) {
        check(a.value, ir::kError);
    }
    return ir::kError;
}

TypeId Checker::inferInstance(TypeId base, const std::vector<TypeId>& patterns, std::span<Arg> args, TypeId expected,
                              Span span) {
    TypeId want = expected;
    if (want != ir::kError && types_->kind(want) == ir::TypeKind::Nullable) {
        want = types_->info(want).elem;
    }
    if (want != ir::kError && types_->info(want).genericBase == base) {
        return want;
    }
    ir::TypeSubst bindings;
    for (std::size_t i = 0; i < args.size() && i < patterns.size(); ++i) {
        TypeId at = checkValue(args[i].value, types_->hasParams(patterns[i]) ? ir::kError : patterns[i]);
        unify(patterns[i], at, bindings);
    }
    std::vector<TypeId> targs;
    for (TypeId p : types_->info(base).typeParams) {
        TypeId bound = ir::kError;
        for (auto& [from, to] : bindings) {
            if (from == p) {
                bound = to;
            }
        }
        if (bound == ir::kError) {
            diags_.error(span, std::format("cannot infer the type arguments of '{}'; annotate the type",
                                           types_->info(base).name));
            return ir::kError;
        }
        targs.push_back(bound);
    }
    for (Arg& a : args) {
        a.value->type = ir::kError; // checked again against the instance's field types
    }
    TypeId inst = types_->instantiate(base, targs);
    checkTypeArgs(inst, span);
    return inst;
}

} // namespace quartz::sema
