#include "sema/SemaImpl.hpp"

#include <algorithm>
#include <format>

namespace quartz::sema {

bool Checker::mentions(TypeId t, const std::vector<TypeId>& ids) const {
    if (t == ir::kError || std::find(ids.begin(), ids.end(), t) != ids.end()) {
        return true;
    }
    if (ids.empty() || !types_->hasParams(t)) {
        return false;
    }
    const ir::TypeInfo& info = types_->info(t);
    auto any = [&](const std::vector<TypeId>& xs) {
        return std::any_of(xs.begin(), xs.end(), [&](TypeId x) { return mentions(x, ids); });
    };
    switch (info.kind) {
    case ir::TypeKind::List:
    case ir::TypeKind::Nullable:
        return mentions(info.elem, ids);
    case ir::TypeKind::Map:
        return mentions(info.key, ids) || mentions(info.elem, ids);
    case ir::TypeKind::Tuple:
        return any(info.elems);
    case ir::TypeKind::Fn:
        return any(info.elems) || mentions(info.result, ids);
    default:
        break;
    }
    if (types_->isResult(t)) {
        return mentions(info.variants[0].payload[0], ids) || mentions(info.variants[1].payload[0], ids);
    }
    return any(info.typeArgs);
}

void Checker::noteCapture(Local* l) {
    for (std::size_t i = l->lambdaDepth; i < lambdas_.size(); ++i) {
        std::vector<Local*>& caps = lambdas_[i].captures;
        if (std::find(caps.begin(), caps.end(), l) == caps.end()) {
            caps.push_back(l);
        }
    }
    l->captured = true;
}

TypeId Checker::checkLambda(LambdaExpr* e, TypeId expected) {
    LambdaHint hint;
    bool hinted = false;
    TypeId want = expected;
    if (want != ir::kError && types_->kind(want) == ir::TypeKind::Nullable) {
        want = types_->info(want).elem;
    }
    if (want != ir::kError && types_->kind(want) == ir::TypeKind::Fn) {
        hint.params = types_->info(want).elems;
        hint.result = types_->info(want).result;
        hinted = true;
    } else if (lambdaHint_) {
        hint = std::move(*lambdaHint_);
        hinted = true;
    }
    lambdaHint_.reset();
    if (hinted && hint.params.size() != e->params.size()) {
        diags_.error(e->span, std::format("expected a function with {} parameter(s), found a lambda with {}",
                                          hint.params.size(), e->params.size()));
        hinted = false;
    }
    if (atDepth_ > 0) {
        diags_.error(e->span, "a lambda cannot be created inside 'at'");
    }
    auto* info = prog_->fnStorage.emplace_back(std::make_unique<FnInfo>()).get();
    info->name = currentFn_ != nullptr && !currentFn_->name.empty() ? currentFn_->name : "<lambda>";
    info->selfLocal = currentFn_ != nullptr ? currentFn_->selfLocal : nullptr;
    bool infer = !hinted || mentions(hint.result, hint.unknown);
    info->result = infer ? ir::kError : hint.result;
    std::vector<TypeId> params;
    for (std::size_t i = 0; i < e->params.size(); ++i) {
        Param& p = e->params[i];
        TypeId pt = ir::kError;
        if (p.type != nullptr) {
            pt = resolveType(p.type);
        } else if (hinted && !mentions(hint.params[i], hint.unknown)) {
            pt = hint.params[i];
        } else {
            diags_.error(p.span,
                         std::format("cannot infer the type of lambda parameter '{}'; write '({}: Type) => ...'",
                                     str(p.name), str(p.name)));
        }
        params.push_back(pt);
    }
    FnInfo* savedFn = currentFn_;
    int savedLoops = loopDepth_;
    lambdas_.push_back({info, {}, infer, false});
    currentFn_ = info;
    loopDepth_ = 0;
    pushScope();
    for (std::size_t i = 0; i < e->params.size(); ++i) {
        Param& p = e->params[i];
        for (std::size_t j = 0; j < i; ++j) {
            if (e->params[j].name == p.name) {
                diags_.error(p.span, std::format("duplicate parameter '{}'", str(p.name)));
            }
        }
        p.local = declareLocal(p.name, params[i], false, p.span);
        p.local->param = true;
    }
    TypeId bodyT = ir::kVoid;
    TypeId bodyWant = infer ? ir::kError : info->result;
    if (e->body->kind == ExprKind::Block) {
        bodyT = checkBlock(static_cast<BlockExpr*>(e->body), bodyWant, infer || info->result != ir::kVoid);
        e->body->type = bodyT;
    } else {
        bodyT = check(e->body, bodyWant);
    }
    popScope();
    currentFn_ = savedFn;
    loopDepth_ = savedLoops;
    LambdaScope scope = std::move(lambdas_.back());
    lambdas_.pop_back();
    TypeId result = info->result;
    if (infer && !scope.sawReturn) {
        result = bodyT == ir::kNever ? ir::kVoid : bodyT;
        if (result == ir::kNull) {
            diags_.error(e->body->span, "cannot infer a type from 'null'; annotate the expected function type");
            result = ir::kError;
        }
    } else if (result != ir::kVoid && result != ir::kError && bodyT != ir::kNever) {
        if (bodyT == ir::kVoid && scope.sawReturn) {
            diags_.error(e->span, std::format("this lambda may end without returning a '{}'", typeName(result)));
        } else {
            expectAssignable(bodyT, result, e->body->span);
        }
    }
    info->result = result;
    info->params = params;
    e->info = info;
    e->captures = arena_.copy(scope.captures);
    if (std::find(params.begin(), params.end(), ir::kError) != params.end() || result == ir::kError) {
        return ir::kError;
    }
    return types_->fn(params, result);
}

bool Checker::lambdaReturn(ReturnStmt* r) {
    if (lambdas_.empty() || currentFn_ != lambdas_.back().info || !lambdas_.back().inferResult) {
        return false;
    }
    LambdaScope& scope = lambdas_.back();
    TypeId t =
        r->value != nullptr ? checkValue(r->value, scope.sawReturn ? currentFn_->result : ir::kError) : ir::kVoid;
    if (!scope.sawReturn) {
        scope.sawReturn = true;
        if (t == ir::kNull) {
            diags_.error(r->span, "cannot infer a type from 'null'; annotate the expected function type");
            t = ir::kError;
        }
        currentFn_->result = t == ir::kNever ? ir::kError : t;
    } else if (currentFn_->result == ir::kVoid && t != ir::kVoid) {
        diags_.error(r->span, "this lambda returned no value before");
    } else if (currentFn_->result != ir::kVoid && t == ir::kVoid) {
        diags_.error(r->span, std::format("missing return value of type '{}'", typeName(currentFn_->result)));
    } else {
        expectAssignable(t, currentFn_->result, r->value->span);
    }
    return true;
}

TypeId Checker::checkFunctionValue(IdentExpr* e, FnInfo* fn, TypeId expected) {
    if (fn->isHost) {
        diags_.error(e->span, std::format("host function '{}' cannot be used as a value yet; wrap it in a lambda",
                                          str(e->name)));
        return ir::kError;
    }
    if (fn->isSuspend) {
        diags_.error(e->span, std::format("suspend function '{}' cannot be used as a value", str(e->name)));
        return ir::kError;
    }
    TypeId pattern = types_->fn(fn->params, fn->result);
    ir::TypeSubst bindings;
    if (fn->generic()) {
        std::size_t own = fn->typeParams.size() - fn->decl->typeParams.size();
        if (!e->typeArgs.empty()) {
            if (e->typeArgs.size() != fn->decl->typeParams.size()) {
                diags_.error(e->span,
                             std::format("'{}' takes {} type argument(s)", fn->name, fn->decl->typeParams.size()));
                return ir::kError;
            }
            for (std::size_t i = 0; i < e->typeArgs.size(); ++i) {
                bindings.emplace_back(fn->typeParams[own + i], resolveType(e->typeArgs[i]));
            }
        } else {
            TypeId want = expected;
            if (want != ir::kError && types_->kind(want) == ir::TypeKind::Nullable) {
                want = types_->info(want).elem;
            }
            if (want != ir::kError && types_->kind(want) == ir::TypeKind::Fn) {
                unify(pattern, want, bindings);
            }
        }
        std::vector<std::uint32_t> args;
        for (std::size_t k = 0; k < fn->typeParams.size(); ++k) {
            auto it = std::find_if(bindings.begin(), bindings.end(),
                                   [&](const auto& b) { return b.first == fn->typeParams[k]; });
            if (it == bindings.end()) {
                diags_.error(e->span,
                             std::format("cannot infer the type arguments of '{}'; write them with ::<>", fn->name));
                return ir::kError;
            }
            for (TraitInfo* t : fn->bounds[k]) {
                if (!implements(it->second, t)) {
                    diags_.error(e->span, std::format("'{}' does not implement '{}'", typeName(it->second), t->name));
                }
            }
            args.push_back(it->second);
        }
        e->fnTypeArgs = arena_.copy(args);
    }
    e->ref = IdentRef::Function;
    e->fn = fn;
    noteFnUse(fn);
    return types_->subst(pattern, bindings);
}

TypeId Checker::checkIndirectCall(CallExpr* call, TypeId fnType) {
    if (fnType == ir::kError || types_->kind(fnType) != ir::TypeKind::Fn) {
        if (fnType != ir::kError) {
            diags_.error(call->callee->span, std::format("a value of type '{}' cannot be called", typeName(fnType)));
        }
        for (Arg& a : call->args) {
            check(a.value, ir::kError);
        }
        return ir::kError;
    }
    std::vector<TypeId> params = types_->info(fnType).elems;
    TypeId result = types_->info(fnType).result;
    for (Arg& a : call->args) {
        if (a.name.valid()) {
            diags_.error(a.value->span, "arguments to a function value cannot be named");
            a.name = Symbol{};
        }
    }
    call->callKind = CallKind::Indirect;
    bindArgs(call, params, {}, {}, 0);
    return result;
}

TypeId Checker::fnField(CallExpr* call, MemberExpr* callee, TypeId recv) {
    const ir::TypeInfo& info = types_->info(recv);
    if (info.kind != ir::TypeKind::Struct) {
        return ir::kError;
    }
    for (std::uint32_t i = 0; i < info.fields.size(); ++i) {
        TypeId ft = info.fields[i].type;
        if (info.fields[i].name != str(callee->name) || types_->kind(ft) != ir::TypeKind::Fn) {
            continue;
        }
        if (call->safeCall) {
            diags_.error(callee->span, "a function held in a nullable value is called after 'if let'");
        }
        callee->fieldIndex = i;
        callee->type = ft;
        return checkIndirectCall(call, ft);
    }
    return ir::kError;
}

Local* Checker::assignedLocal(Expr* target) const {
    switch (target->kind) {
    case ExprKind::Ident: {
        auto* id = static_cast<IdentExpr*>(target);
        return id->ref == IdentRef::Local ? id->local : nullptr;
    }
    case ExprKind::SelfRef:
        return static_cast<SelfExpr*>(target)->local;
    case ExprKind::Member: {
        auto* m = static_cast<MemberExpr*>(target);
        ir::TypeKind k = m->object->type < types_->size() ? types_->kind(m->object->type) : ir::TypeKind::Error;
        return k == ir::TypeKind::Struct || k == ir::TypeKind::Tuple ? assignedLocal(m->object) : nullptr;
    }
    default:
        return nullptr;
    }
}

void Checker::checkCapturedAssign(Expr* target, Span span) {
    Local* l = assignedLocal(target);
    if (l == nullptr) {
        return;
    }
    if (l->lambdaDepth < lambdas_.size()) {
        diags_.error(
            span, std::format("cannot assign '{}' inside a lambda: lambdas capture copies of variables", str(l->name)));
    } else if (l->captured) {
        diags_.error(span, std::format("cannot assign '{}' after a lambda captured it: the lambda keeps the old "
                                       "value; copy it into a new variable before capturing",
                                       str(l->name)));
    }
}

void Checker::checkFunctionTypes() {
    for (TypeId s : prog_->states) {
        syntax::StructDecl* decl = prog_->structDecls[s];
        const ir::TypeInfo& info = types_->info(s);
        for (std::size_t i = 0; i < info.fields.size(); ++i) {
            if (types_->containsFn(info.fields[i].type)) {
                diags_.error(decl != nullptr && i < decl->fields.size() ? decl->fields[i].span : Span{},
                             "a state cannot hold functions: they would not survive a reload");
            }
        }
    }
    for (FnInfo* imp : prog_->imports) {
        bool fn = types_->containsFn(imp->result);
        for (TypeId p : imp->params) {
            fn = fn || types_->containsFn(p);
        }
        if (fn) {
            diags_.error(imp->decl != nullptr ? imp->decl->span : Span{},
                         std::format("host function '{}' cannot take or return functions", imp->name));
        }
    }
}

} // namespace quartz::sema
