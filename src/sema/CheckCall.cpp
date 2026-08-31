#include "sema/SemaImpl.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace quartz::sema {

using ir::Intrinsic;

std::optional<BuiltinSig> Checker::builtinMethod(TypeId recv, std::string_view name) {
    if (std::optional<BuiltinSig> extra = extraBuiltinMethod(recv, name)) {
        return extra;
    }
    const ir::TypeInfo& info = types_->info(recv);
    if (recv == ir::kStr) {
        static const std::pair<std::string_view, BuiltinSig> table[] = {
            {"len", {Intrinsic::StrLen, {}, ir::kI64}},
            {"lower", {Intrinsic::StrLower, {}, ir::kStr}},
            {"upper", {Intrinsic::StrUpper, {}, ir::kStr}},
            {"contains", {Intrinsic::StrContains, {ir::kStr}, ir::kBool}},
            {"startsWith", {Intrinsic::StrStartsWith, {ir::kStr}, ir::kBool}},
            {"endsWith", {Intrinsic::StrEndsWith, {ir::kStr}, ir::kBool}},
            {"slice", {Intrinsic::StrSlice, {ir::kI64, ir::kI64}, ir::kStr}},
            {"byteAt", {Intrinsic::StrByteAt, {ir::kI64}, ir::kI64}},
        };
        for (const auto& [n, sig] : table) {
            if (n == name) {
                return sig;
            }
        }
        if (name == "find") {
            return BuiltinSig{Intrinsic::StrFind, {ir::kStr}, types_->nullable(ir::kI64)};
        }
        return std::nullopt;
    }
    if (info.kind == ir::TypeKind::List) {
        TypeId elem = info.elem;
        if (name == "len") {
            return BuiltinSig{Intrinsic::ListLen, {}, ir::kI64};
        }
        if (name == "push") {
            return BuiltinSig{Intrinsic::ListPush, {elem}, ir::kVoid};
        }
        if (name == "pop") {
            return BuiltinSig{Intrinsic::ListPop, {}, types_->nullable(elem)};
        }
        if (name == "clear") {
            return BuiltinSig{Intrinsic::ListClear, {}, ir::kVoid};
        }
        if (name == "join" && elem == ir::kStr) {
            return BuiltinSig{Intrinsic::ListJoin, {ir::kStr}, ir::kStr};
        }
        return std::nullopt;
    }
    if (info.kind == ir::TypeKind::Map) {
        if (name == "len") {
            return BuiltinSig{Intrinsic::MapLen, {}, ir::kI64};
        }
        if (name == "get") {
            return BuiltinSig{Intrinsic::MapGet, {info.key}, types_->nullable(info.elem)};
        }
        if (name == "set") {
            return BuiltinSig{Intrinsic::MapSet, {info.key, info.elem}, ir::kVoid};
        }
        if (name == "has") {
            return BuiltinSig{Intrinsic::MapHas, {info.key}, ir::kBool};
        }
        if (name == "remove") {
            return BuiltinSig{Intrinsic::MapRemove, {info.key}, ir::kBool};
        }
        return std::nullopt;
    }
    if (recv == ir::kF64) {
        static const std::pair<std::string_view, Intrinsic> table[] = {
            {"sqrt", Intrinsic::MathSqrt}, {"floor", Intrinsic::MathFloor}, {"ceil", Intrinsic::MathCeil},
            {"abs", Intrinsic::MathAbsF},  {"sin", Intrinsic::MathSin},     {"cos", Intrinsic::MathCos},
        };
        for (const auto& [n, op] : table) {
            if (n == name) {
                return BuiltinSig{op, {}, ir::kF64};
            }
        }
    }
    if (recv == ir::kI64 && name == "abs") {
        return BuiltinSig{Intrinsic::MathAbsI, {}, ir::kI64};
    }
    return std::nullopt;
}

bool Checker::bindArgs(CallExpr* call, const std::vector<TypeId>& params, const std::vector<Symbol>& names,
                       const std::vector<Expr*>& defaults, std::size_t skip) {
    std::size_t n = params.size() - skip;
    std::vector<Expr*> ordered(n, nullptr);
    bool ok = true;
    bool sawNamed = false;
    std::size_t positional = 0;
    for (Arg& a : call->args) {
        std::size_t slot = n;
        if (a.name.valid()) {
            sawNamed = true;
            for (std::size_t i = 0; i < n; ++i) {
                if (names.size() > i + skip && names[i + skip] == a.name) {
                    slot = i;
                }
            }
            if (slot == n) {
                diags_.error(a.value->span, std::format("no parameter named '{}'", str(a.name)));
                check(a.value, ir::kError);
                ok = false;
                continue;
            }
        } else {
            if (sawNamed) {
                diags_.error(a.value->span, "positional arguments must come before named ones");
                ok = false;
            }
            slot = positional++;
            if (slot >= n) {
                diags_.error(a.value->span, std::format("too many arguments: expected {}", n));
                check(a.value, ir::kError);
                ok = false;
                continue;
            }
        }
        if (ordered[slot] != nullptr) {
            diags_.error(a.value->span, "argument given twice");
            ok = false;
        }
        ordered[slot] = a.value;
        TypeId pt = params[slot + skip];
        expectAssignable(checkValue(a.value, pt), pt, a.value->span);
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (ordered[i] == nullptr) {
            Expr* d = defaults.size() > i + skip ? defaults[i + skip] : nullptr;
            if (d == nullptr) {
                std::string pname = names.size() > i + skip ? std::string(str(names[i + skip])) : std::to_string(i);
                diags_.error(call->span, std::format("missing argument '{}'", pname));
                ok = false;
            } else {
                if (d->type == ir::kError && !defaultsInProgress_.insert(d).second) {
                    diags_.error(d->span, "this default value depends on itself");
                    ok = false;
                } else if (d->type == ir::kError) {
                    auto saved = std::move(scopes_);
                    scopes_.clear();
                    expectAssignable(checkValue(d, params[i + skip]), params[i + skip], d->span);
                    scopes_ = std::move(saved);
                    defaultsInProgress_.erase(d);
                }
                ordered[i] = d;
            }
        }
    }
    call->ordered = arena_.copy(ordered);
    return ok;
}

TypeId Checker::finishCall(CallExpr* call, FnInfo* fn, Expr* receiver) {
    noteFnUse(fn);
    if (fn->deprecated && fn != currentFn_) {
        diags_.warning(call->callee->span, rule::kDeprecated,
                       fn->deprecation.empty() ? std::format("'{}' is deprecated", fn->name)
                                               : std::format("'{}' is deprecated: {}", fn->name, fn->deprecation));
    }
    if (fn->isSuspend && (currentFn_ == nullptr || !currentFn_->isSuspend)) {
        diags_.error(call->span, std::format("suspend function '{}' can only be called from a suspend function",
                                             fn->name));
    }
    if (fn->generic()) {
        ir::TypeSubst bindings;
        if (receiver != nullptr && fn->hasSelf) {
            TypeId rt = receiver->type;
            if (call->safeCall && types_->kind(rt) == ir::TypeKind::Nullable) {
                rt = types_->info(rt).elem;
            }
            unify(fn->owner, rt, bindings);
        }
        std::span<TypeExpr*> explicitArgs;
        if (call->callee->kind == ExprKind::Ident) {
            explicitArgs = static_cast<IdentExpr*>(call->callee)->typeArgs;
        }
        std::size_t own = fn->typeParams.size() - (fn->decl != nullptr ? fn->decl->typeParams.size() : 0);
        if (!explicitArgs.empty()) {
            if (explicitArgs.size() != fn->decl->typeParams.size()) {
                diags_.error(call->span, std::format("'{}' takes {} type argument(s)", fn->name,
                                                     fn->decl->typeParams.size()));
            }
            for (std::size_t i = 0; i < explicitArgs.size() && own + i < fn->typeParams.size(); ++i) {
                bindings.emplace_back(fn->typeParams[own + i], resolveType(explicitArgs[i]));
            }
        }
        return finishGenericCall(call, fn, receiver, std::move(bindings));
    }
    call->fn = fn;
    call->receiver = receiver;
    call->callKind = fn->isHost ? CallKind::Host : CallKind::Function;
    bindArgs(call, fn->params, fn->paramNames, fn->defaults, receiver != nullptr ? 1 : 0);
    return fn->result;
}

TypeId Checker::checkConstruct(CallExpr* call, TypeId type) {
    const ir::TypeInfo& info = types_->info(type);
    if (info.kind != ir::TypeKind::Struct) {
        diags_.error(call->span, std::format("'{}' cannot be constructed", typeName(type)));
        for (Arg& a : call->args) {
            check(a.value, ir::kError);
        }
        return ir::kError;
    }
    call->callKind = CallKind::Construct;
    std::vector<TypeId> params;
    std::vector<Symbol> names;
    std::vector<Expr*> defaults;
    StructDecl* decl = structDeclOf(type);
    for (std::size_t i = 0; i < info.fields.size(); ++i) {
        params.push_back(info.fields[i].type);
        names.push_back(decl->fields[i].name);
        defaults.push_back(decl->fields[i].defaultValue);
    }
    bindArgs(call, params, names, defaults, 0);
    return type;
}

TypeId Checker::checkBuiltinFunction(CallExpr* call, Symbol name, TypeId expected, bool& found, bool qualified) {
    found = true;
    std::string_view n = str(name);
    static constexpr std::string_view kMath[] = {"sqrt", "floor", "ceil", "sin", "cos", "pow", "abs", "min", "max"};
    auto inMath = [](std::string_view s) {
        return std::find(std::begin(kMath), std::end(kMath), s) != std::end(kMath) || isExtraMathFunction(s);
    };
    bool isMath = false;
    if (qualified) {
        isMath = inMath(n);
    } else if (std::string_view original; mathImported(name, original)) {
        isMath = inMath(original);
        n = isMath ? original : n;
    } else if (inMath(n)) {
        diags_.error(call->callee->span,
                     std::format("'{}' is in {}; import it with 'import {}.{}'", n, kMathPackage, kMathPackage, n));
        for (Arg& a : call->args) {
            check(a.value, ir::kError);
        }
        return ir::kError;
    }
    if (qualified && !isMath) {
        found = false;
        return ir::kError;
    }
    auto* ident = call->callee->kind == ExprKind::Ident ? static_cast<IdentExpr*>(call->callee) : nullptr;
    auto unaryF64 = [&](Intrinsic op) {
        call->callKind = CallKind::Builtin;
        call->builtin = static_cast<std::uint16_t>(op);
        bindArgs(call, {ir::kF64}, {}, {}, 0);
        return ir::kF64;
    };
    if (TypeId extra = ir::kError; checkExtraBuiltinFunction(call, n, expected, extra)) {
        return extra;
    }
    if (n == "sqrt") {
        return unaryF64(Intrinsic::MathSqrt);
    }
    if (n == "floor") {
        return unaryF64(Intrinsic::MathFloor);
    }
    if (n == "ceil") {
        return unaryF64(Intrinsic::MathCeil);
    }
    if (n == "sin") {
        return unaryF64(Intrinsic::MathSin);
    }
    if (n == "cos") {
        return unaryF64(Intrinsic::MathCos);
    }
    if (n == "pow") {
        call->callKind = CallKind::Builtin;
        call->builtin = static_cast<std::uint16_t>(Intrinsic::MathPow);
        bindArgs(call, {ir::kF64, ir::kF64}, {}, {}, 0);
        return ir::kF64;
    }
    if (n == "panic") {
        call->callKind = CallKind::Builtin;
        call->builtin = static_cast<std::uint16_t>(Intrinsic::Panic);
        bindArgs(call, {ir::kStr}, {}, {}, 0);
        return ir::kNever;
    }
    if (n == "abs" || n == "min" || n == "max") {
        std::size_t arity = n == "abs" ? 1 : 2;
        if (call->args.size() != arity) {
            diags_.error(call->span, std::format("'{}' takes {} argument(s)", n, arity));
            return ir::kError;
        }
        TypeId t = checkValue(call->args[0].value, expected);
        if (t != ir::kI64 && t != ir::kF64 && t != ir::kError) {
            diags_.error(call->span, std::format("'{}' works on int and float", n));
            return ir::kError;
        }
        bool isF = t == ir::kF64;
        Intrinsic op = n == "abs" ? (isF ? Intrinsic::MathAbsF : Intrinsic::MathAbsI)
                       : n == "min" ? (isF ? Intrinsic::MathMinF : Intrinsic::MathMinI)
                                    : (isF ? Intrinsic::MathMaxF : Intrinsic::MathMaxI);
        call->callKind = CallKind::Builtin;
        call->builtin = static_cast<std::uint16_t>(op);
        std::vector<TypeId> params(arity, t);
        bindArgs(call, params, {}, {}, 0);
        return t;
    }
    if (n == "Ok" || n == "Err") {
        TypeId want = expected;
        if (want != ir::kError && types_->kind(want) == ir::TypeKind::Nullable) {
            want = types_->info(want).elem;
        }
        if (want == ir::kError || !types_->isResult(want)) {
            diags_.error(call->span, std::format("cannot infer the Result type of '{}(...)'; annotate the type", n));
            for (Arg& a : call->args) {
                check(a.value, ir::kError);
            }
            return ir::kError;
        }
        std::uint32_t v = n == "Ok" ? 0 : 1;
        call->callKind = CallKind::Variant;
        call->variant = v;
        bindArgs(call, types_->info(want).variants[v].payload, {}, {}, 0);
        return want;
    }
    if (n == "Map" || n == "List") {
        TypeId t = ir::kError;
        if (!ident->typeArgs.empty()) {
            std::size_t want = n == "Map" ? 2 : 1;
            if (ident->typeArgs.size() != want) {
                diags_.error(call->span, std::format("'{}' takes {} type argument(s)", n, want));
                return ir::kError;
            }
            t = n == "Map" ? types_->map(resolveType(ident->typeArgs[0]), resolveType(ident->typeArgs[1]))
                           : types_->list(resolveType(ident->typeArgs[0]));
        } else {
            TypeId want = expected;
            if (want != ir::kError && types_->kind(want) == ir::TypeKind::Nullable) {
                want = types_->info(want).elem;
            }
            ir::TypeKind k = want == ir::kError ? ir::TypeKind::Error : types_->kind(want);
            if ((n == "Map" && k != ir::TypeKind::Map) || (n == "List" && k != ir::TypeKind::List)) {
                diags_.error(call->span, std::format("cannot infer the type of '{}()'; write '{}::<...>()'", n, n));
                return ir::kError;
            }
            t = want;
        }
        if (!call->args.empty()) {
            diags_.error(call->span, std::format("'{}()' takes no arguments", n));
        }
        call->callKind = CallKind::Builtin;
        call->builtin = static_cast<std::uint16_t>(n == "Map" ? Intrinsic::MapNew : Intrinsic::ListNew);
        return t;
    }
    found = false;
    return ir::kError;
}

TypeId Checker::checkBuiltinMethod(CallExpr* call, TypeId recvType, Symbol name, TypeId expected) {
    (void)expected;
    std::optional<BuiltinSig> sig = builtinMethod(recvType, str(name));
    if (!sig) {
        diags_.error(call->callee->span, std::format("'{}' has no method '{}'", typeName(recvType), str(name)));
        for (Arg& a : call->args) {
            check(a.value, ir::kError);
        }
        return ir::kError;
    }
    call->callKind = CallKind::Builtin;
    call->builtin = static_cast<std::uint16_t>(sig->op);
    bindArgs(call, sig->params, {}, {}, 0);
    return sig->result;
}

// `Type.name(...)` names a function of an `impl Type` block declared without `self` (D142).
FnInfo* Checker::associatedFn(TypeId type, MemberExpr* callee) {
    const ir::TypeInfo& info = types_->info(type);
    TypeId base = info.genericBase != 0 ? info.genericBase : type;
    auto& table = types_->isGenericBase(base) ? genericMethods_ : methods_;
    auto it = table.find({base, callee->name.id});
    if (it == table.end()) {
        return nullptr;
    }
    FnInfo* fn = it->second;
    if (fn->hasSelf) {
        diags_.error(callee->span, std::format("'{}' is a method; call it on a value", fn->name));
        return nullptr;
    }
    const FnDecl* d = fn->decl;
    bool visible =
        d == nullptr || d->visibility == Visibility::Pub || d->span.file == currentFile_ ||
        (d->visibility == Visibility::Internal && files_[d->span.file].package == files_[currentFile_].package);
    if (!visible) {
        diags_.error(callee->span, std::format("'{}' is not public", fn->name));
    }
    return fn;
}

TypeId Checker::checkMethodCall(CallExpr* call, MemberExpr* callee, TypeId expected) {
    if (std::string pkg; packagePath(callee->object, pkg)) {
        if (pkg == kMathPackage) {
            bool found = false;
            TypeId t = checkBuiltinFunction(call, callee->name, expected, found, true);
            if (found) {
                return t;
            }
        }
        if (auto* fn = static_cast<FnInfo*>(findIn(pkg, callee->name, NameKind::Function))) {
            return finishCall(call, fn, nullptr);
        }
        if (auto* te = static_cast<TypeEntry*>(findIn(pkg, callee->name, NameKind::Type));
            te != nullptr && !types_->isGenericBase(te->type)) {
            return checkConstruct(call, te->type);
        }
        diags_.error(callee->span, std::format("package '{}' has no public function or type '{}'", pkg, str(callee->name)));
        for (Arg& a : call->args) {
            check(a.value, ir::kError);
        }
        return ir::kError;
    }
    // `Enum.Variant(payload)`, also `package.Enum.Variant(payload)`
    if (!callee->safe) {
        auto* id = callee->object->kind == ExprKind::Ident ? static_cast<IdentExpr*>(callee->object) : nullptr;
        TypeId et = ir::kError;
        if (id != nullptr && findLocal(id->name) == nullptr && findGlobal(id->name) == nullptr) {
            et = lookupTypeName(id->name, id->span, false);
        } else if (id == nullptr) {
            et = qualifiedType(callee->object);
        }
        bool variant = false;
        if (et != ir::kError && types_->kind(et) == ir::TypeKind::Enum) {
            for (const ir::VariantInfo& v : types_->info(et).variants) {
                variant = variant || v.name == str(callee->name);
            }
        }
        if (et != ir::kError && !variant) {
            std::size_t errors = diags_.errorCount();
            if (FnInfo* fn = associatedFn(et, callee)) {
                return finishCall(call, fn, nullptr);
            }
            if (types_->kind(et) != ir::TypeKind::Enum || errors != diags_.errorCount()) {
                if (errors == diags_.errorCount()) {
                    diags_.error(callee->span,
                                 std::format("'{}' has no function '{}'", typeName(et), str(callee->name)));
                }
                for (Arg& a : call->args) {
                    check(a.value, ir::kError);
                }
                return ir::kError;
            }
        }
        if (et != ir::kError && types_->kind(et) == ir::TypeKind::Enum) {
            const ir::TypeInfo& info = types_->info(et);
            for (std::uint32_t v = 0; v < info.variants.size(); ++v) {
                if (info.variants[v].name == str(callee->name)) {
                    call->callKind = CallKind::Variant;
                    call->variant = v;
                    if (types_->isGenericBase(et)) {
                        et = inferInstance(et, info.variants[v].payload, call->args, expected, call->span);
                        if (et == ir::kError) {
                            return et;
                        }
                    }
                    std::vector<TypeId> payload = types_->info(et).variants[v].payload;
                    bindArgs(call, payload, {}, {}, 0);
                    return et;
                }
            }
            diags_.error(callee->span, std::format("'{}' has no variant '{}'", typeName(et), str(callee->name)));
            return ir::kError;
        }
    }
    TypeId recv = call->safeCall && !callee->safe ? types_->info(callee->object->type).elem
                                                   : checkValue(callee->object, ir::kError);
    callee->type = ir::kError;
    if (callee->safe && recv != ir::kError) {
        if (types_->kind(recv) != ir::TypeKind::Nullable) {
            diags_.error(callee->span, std::format("'?.' needs a nullable value, found '{}'", typeName(recv)));
            return ir::kError;
        }
        call->safeCall = true;
        callee->safe = false;
        TypeId inner = checkMethodCall(call, callee, expected);
        callee->safe = true;
        call->innerType = inner;
        if (inner == ir::kVoid || inner == ir::kError || types_->kind(inner) == ir::TypeKind::Nullable) {
            return inner;
        }
        return types_->nullable(inner);
    }
    if (recv == ir::kError) {
        for (Arg& a : call->args) {
            check(a.value, ir::kError);
        }
        return ir::kError;
    }
    if (types_->kind(recv) == ir::TypeKind::Param) {
        return checkTraitCall(call, callee, recv);
    }
    if (types_->info(recv).genericBase != 0 && !methods_.contains({recv, callee->name.id})) {
        auto git = genericMethods_.find({types_->info(recv).genericBase, callee->name.id});
        if (git != genericMethods_.end()) {
            if (!git->second->hasSelf) {
                diags_.error(callee->span, std::format("'{}' is not a method", git->second->name));
                return ir::kError;
            }
            return finishCall(call, git->second, callee->object);
        }
    }
    auto it = methods_.find({recv, callee->name.id});
    if (it != methods_.end()) {
        FnInfo* fn = it->second;
        if (!fn->hasSelf) {
            diags_.error(callee->span, std::format("'{}' is not a method; call it as '{}'", fn->name, fn->name));
            return ir::kError;
        }
        return finishCall(call, fn, callee->object);
    }
    if (TypeId ft = fnField(call, callee, recv); ft != ir::kError || callee->type != ir::kError) {
        return ft;
    }
    call->receiver = callee->object;
    std::string_view mname = str(callee->name);
    bool isResult = types_->isResult(recv);
    bool isNullable = types_->kind(recv) == ir::TypeKind::Nullable;
    if (isResult || isNullable) {
        TypeId inner = isResult ? types_->info(recv).variants[0].payload[0] : types_->info(recv).elem;
        call->callKind = CallKind::Special;
        if (isResult && (mname == "isOk" || mname == "isErr")) {
            call->builtin = static_cast<std::uint16_t>(mname == "isOk" ? SpecialOp::ResultIsOk : SpecialOp::ResultIsErr);
            bindArgs(call, {}, {}, {}, 0);
            return ir::kBool;
        }
        if (mname == "unwrap") {
            call->builtin = static_cast<std::uint16_t>(isResult ? SpecialOp::ResultUnwrap : SpecialOp::NullUnwrap);
            bindArgs(call, {}, {}, {}, 0);
            return inner;
        }
        if (mname == "unwrapOr") {
            call->builtin = static_cast<std::uint16_t>(isResult ? SpecialOp::ResultUnwrapOr : SpecialOp::NullUnwrapOr);
            bindArgs(call, {inner}, {}, {}, 0);
            return inner;
        }
        diags_.error(callee->span, std::format("'{}' has no method '{}'", typeName(recv), mname));
        return ir::kError;
    }
    if (types_->isArray(recv)) {
        return arrayMethod(call, callee, recv);
    }
    if (TypeId hof = ir::kError; checkListFunctionMethod(call, recv, callee->name, hof)) {
        return hof;
    }
    if (TypeId t = langMethodCall(call, callee, recv); t != ir::kError) {
        return t;
    }
    if (TypeId t = mapListCall(call, callee, recv); t != ir::kError) {
        return t;
    }
    return checkBuiltinMethod(call, recv, callee->name, expected);
}

TypeId Checker::checkCall(CallExpr* e, TypeId expected) {
    if (e->callee->kind == ExprKind::Member) {
        return checkMethodCall(e, static_cast<MemberExpr*>(e->callee), expected);
    }
    if (e->callee->kind == ExprKind::Ident) {
        auto* id = static_cast<IdentExpr*>(e->callee);
        if (findLocal(id->name) != nullptr) {
            return checkIndirectCall(e, check(e->callee, ir::kError));
        }
        if (FnInfo* target = findFunction(id->name)) {
            id->ref = IdentRef::Function;
            id->fn = target;
            return finishCall(e, target, nullptr);
        }
        if (findGlobal(id->name) != nullptr) {
            return checkIndirectCall(e, check(e->callee, ir::kError));
        }
        bool found = false;
        TypeId bt = checkBuiltinFunction(e, id->name, expected, found);
        if (found) {
            return bt;
        }
        TypeId tt = lookupTypeName(id->name, id->span, false);
        if (tt != ir::kError) {
            id->ref = IdentRef::Type;
            if (types_->isGenericBase(tt)) {
                const ir::TypeInfo& base = types_->info(tt);
                if (!id->typeArgs.empty()) {
                    std::vector<TypeId> args;
                    for (TypeExpr* a : id->typeArgs) {
                        args.push_back(resolveType(a));
                    }
                    if (args.size() != base.typeParams.size()) {
                        diags_.error(id->span, std::format("'{}' takes {} type argument(s)", base.name,
                                                           base.typeParams.size()));
                        return ir::kError;
                    }
                    tt = types_->instantiate(tt, args);
                    checkTypeArgs(tt, id->span);
                } else {
                    std::vector<TypeId> fields;
                    for (const ir::Field& f : base.fields) {
                        fields.push_back(f.type);
                    }
                    tt = inferInstance(tt, fields, e->args, expected, e->span);
                    if (tt == ir::kError) {
                        return tt;
                    }
                }
            }
            return checkConstruct(e, tt);
        }
        diags_.error(id->span, std::format("unknown function '{}'", str(id->name)));
        for (Arg& a : e->args) {
            check(a.value, ir::kError);
        }
        return ir::kError;
    }
    return checkIndirectCall(e, checkValue(e->callee, ir::kError));
}

TypeId Checker::checkMember(MemberExpr* e, TypeId expected) {
    if (std::string pkg; !e->safe && packagePath(e->object, pkg)) {
        auto* g = static_cast<GlobalInfo*>(findIn(pkg, e->name, NameKind::Global));
        if (g == nullptr) {
            diags_.error(e->span, std::format("package '{}' has no public constant '{}'", pkg, str(e->name)));
            return ir::kError;
        }
        e->global = g;
        return checkGlobal(g, e->span);
    }
    if (!e->safe) {
        auto* id = e->object->kind == ExprKind::Ident ? static_cast<IdentExpr*>(e->object) : nullptr;
        TypeId et = ir::kError;
        if (id != nullptr && findLocal(id->name) == nullptr && findGlobal(id->name) == nullptr) {
            et = lookupTypeName(id->name, id->span, false);
        } else if (id == nullptr) {
            et = qualifiedType(e->object);
        }
        if (et != ir::kError && types_->isGenericBase(et) && types_->kind(et) == ir::TypeKind::Enum) {
            TypeId want = expected;
            if (want != ir::kError && types_->kind(want) == ir::TypeKind::Nullable) {
                want = types_->info(want).elem;
            }
            if (want != ir::kError && types_->info(want).genericBase == et) {
                et = want;
            } else {
                diags_.error(e->span, std::format("cannot infer the type arguments of '{}'; annotate the type",
                                                  types_->info(et).name));
                return ir::kError;
            }
        }
        if (et != ir::kError) {
            const ir::TypeInfo& info = types_->info(et);
            if (info.kind == ir::TypeKind::Enum) {
                for (std::uint32_t v = 0; v < info.variants.size(); ++v) {
                    if (info.variants[v].name == str(e->name)) {
                        if (!info.variants[v].payload.empty()) {
                            diags_.error(e->span, std::format("variant '{}' needs its payload", str(e->name)));
                        }
                        e->isVariant = true;
                        e->fieldIndex = v;
                        if (id != nullptr) {
                            id->ref = IdentRef::Type;
                        }
                        return et;
                    }
                }
            }
            diags_.error(e->span, std::format("'{}' has no member '{}'", typeName(et), str(e->name)));
            return ir::kError;
        }
    }
    TypeId ot = checkValue(e->object, ir::kError);
    if (ot == ir::kError) {
        return ot;
    }
    bool wrap = false;
    if (e->safe) {
        if (types_->kind(ot) != ir::TypeKind::Nullable) {
            diags_.error(e->span, std::format("'?.' needs a nullable value, found '{}'", typeName(ot)));
            return ir::kError;
        }
        ot = types_->info(ot).elem;
        wrap = true;
    } else if (types_->kind(ot) == ir::TypeKind::Nullable) {
        diags_.error(e->span, std::format("value of type '{}' may be null; use '?.' or 'if let'", typeName(ot)));
        return ir::kError;
    }
    const ir::TypeInfo& info = types_->info(ot);
    TypeId result = ir::kError;
    if (info.kind == ir::TypeKind::State && atDepth_ > 0) {
        diags_.error(e->span, "state cannot be accessed inside 'at'");
    }
    if (info.kind == ir::TypeKind::Struct || info.kind == ir::TypeKind::State) {
        for (std::uint32_t i = 0; i < info.fields.size(); ++i) {
            if (info.fields[i].name == str(e->name)) {
                e->fieldIndex = i;
                result = info.fields[i].type;
            }
        }
    } else if (info.kind == ir::TypeKind::Tuple && info.array) {
        diags_.error(e->span, std::format("an array has no fields; write '[{}]' to read an element", str(e->name)));
        return ir::kError;
    } else if (info.kind == ir::TypeKind::Tuple) {
        std::string_view n = str(e->name);
        std::uint32_t idx = 0;
        auto res = std::from_chars(n.data(), n.data() + n.size(), idx);
        if (res.ec == std::errc{} && idx < info.elems.size()) {
            e->fieldIndex = idx;
            result = info.elems[idx];
        }
    } else if (info.kind == ir::TypeKind::Handle) {
        auto it = properties_.find({ot, e->name.id});
        if (it != properties_.end()) {
            e->isProperty = true;
            e->getter = it->second.getter;
            result = it->second.type;
        }
    }
    if (result == ir::kError) {
        auto mit = methods_.find({ot, e->name.id});
        if (mit != methods_.end() && mit->second->isProperty) {
            e->isProperty = true;
            e->getter = mit->second->index;
            result = mit->second->result;
            return wrap ? types_->nullable(result) : result;
        }
        if (methods_.contains({ot, e->name.id}) || builtinMethod(ot, str(e->name))) {
            diags_.error(e->span, std::format("'{}' is a method; call it with '()'", str(e->name)));
        } else {
            diags_.error(e->span, std::format("'{}' has no field '{}'", typeName(ot), str(e->name)));
        }
        return ir::kError;
    }
    return wrap ? types_->nullable(result) : result;
}

} // namespace quartz::sema
