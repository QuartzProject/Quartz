#include "sema/SemaImpl.hpp"

#include <format>

namespace quartz::sema {

Checker::Checker(std::span<Module*> modules, Interner& names, Arena& arena, Diagnostics& diags)
    : modules_(modules), names_(names), arena_(arena), diags_(diags), prog_(std::make_unique<Program>()) {
    types_ = &prog_->types;
    prog_->names = &names;
}

bool Checker::hasAttr(std::span<Attribute> attrs, std::string_view name) const {
    for (const Attribute& a : attrs) {
        if (names_.name(a.name) == name) {
            return true;
        }
    }
    return false;
}

void Checker::declareTypes() {
    for (Module* mod : modules_) {
        currentFile_ = mod->file->id();
        for (Decl* d : mod->decls) {
            if (d->kind != DeclKind::Struct && d->kind != DeclKind::State && d->kind != DeclKind::Enum) {
                continue;
            }
            std::size_t savedScopes = typeScopes_.size();
            typeScopes_.clear();
            bool builtin = lookupTypeName(d->name, d->span, false) != ir::kError && findType(d->name) == nullptr;
            typeScopes_.resize(savedScopes);
            if (builtin) {
                diags_.error(d->span, std::format("'{}' is a builtin type name", str(d->name)));
                continue;
            }
            ir::TypeKind kind = ir::TypeKind::Struct;
            if (d->kind == DeclKind::State) {
                kind = ir::TypeKind::State;
            } else if (d->kind == DeclKind::Enum) {
                kind = ir::TypeKind::Enum;
            } else if (hasAttr(d->attrs, "handle")) {
                kind = ir::TypeKind::Handle;
                if (!mod->file->isInterface()) {
                    diags_.error(d->span, "@handle types can only be declared in a host interface (.qzi)");
                }
            }
            TypeEntry entry;
            entry.type = types_->declareNominal(kind, std::string(str(d->name)));
            std::span<TypeParam> tps;
            if (d->kind == DeclKind::Enum) {
                entry.enumDecl = static_cast<EnumDecl*>(d);
                prog_->enumDecls[entry.type] = entry.enumDecl;
                tps = entry.enumDecl->typeParams;
            } else {
                entry.decl = static_cast<StructDecl*>(d);
                prog_->structDecls[entry.type] = entry.decl;
                tps = entry.decl->typeParams;
                if (!tps.empty() && kind != ir::TypeKind::Struct) {
                    diags_.error(d->span, "only structs and enums can be generic");
                    tps = {};
                }
            }
            if (!tps.empty()) {
                typeParamDecls_[entry.type] = {tps, currentFile_};
            }
            for (const TypeParam& tp : tps) {
                TypeId pid = types_->param(std::string(str(tp.name)));
                types_->mutableInfo(entry.type).typeParams.push_back(pid);
                baseParams_[entry.type].emplace_back(tp.name, pid);
            }
            auto* stored = typeEntries_.emplace_back(std::make_unique<TypeEntry>(entry)).get();
            addName(d->name, NameKind::Type, stored, d, currentFile_);
            if (kind == ir::TypeKind::State) {
                prog_->states.push_back(entry.type);
            }
        }
    }
}

syntax::StructDecl* Checker::structDeclOf(TypeId t) {
    const ir::TypeInfo& info = types_->info(t);
    auto it = prog_->structDecls.find(info.genericBase != 0 ? info.genericBase : t);
    return it != prog_->structDecls.end() ? it->second : nullptr;
}

void Checker::completeTypes() {
    for (auto& entryPtr : typeEntries_) {
        TypeEntry& entry = *entryPtr;
        Decl* owner = entry.enumDecl != nullptr ? static_cast<Decl*>(entry.enumDecl) : entry.decl;
        currentFile_ = owner->span.file;
        Symbol name = owner->name;
        typeScopes_.emplace_back();
        for (const auto& [sym, pid] : baseParams_[entry.type]) {
            typeScopes_.back()[sym] = pid;
        }
        struct PopScope {
            std::vector<std::unordered_map<Symbol, TypeId>>& s;
            ~PopScope() { s.pop_back(); }
        } pop{typeScopes_};
        ir::TypeInfo& info = types_->mutableInfo(entry.type);
        if (entry.enumDecl != nullptr) {
            for (const Variant& v : entry.enumDecl->variants) {
                ir::VariantInfo vi;
                vi.name = std::string(str(v.name));
                for (TypeExpr* p : v.payload) {
                    vi.payload.push_back(resolveType(p));
                }
                types_->mutableInfo(entry.type).variants.push_back(std::move(vi));
            }
            types_->mutableInfo(entry.type).complete = true;
            continue;
        }
        bool isHandle = info.kind == ir::TypeKind::Handle;
        std::vector<ir::Field> fields;
        for (const FieldDecl& f : entry.decl->fields) {
            for (const ir::Field& existing : fields) {
                if (existing.name == str(f.name)) {
                    diags_.error(f.span, std::format("duplicate field '{}'", str(f.name)));
                }
            }
            TypeId ft = resolveType(f.type);
            std::string renamed;
            for (const Attribute& a : f.attrs) {
                if (str(a.name) == "key" && derivesCodec(entry.decl)) {
                    continue; // checked with the derive
                }
                if (str(a.name) != "renamed") {
                    diags_.warning(a.span, std::format("unknown field attribute '@{}'", str(a.name)));
                    continue;
                }
                if (a.args.size() != 1 || a.args[0].value->kind != ExprKind::StringLit) {
                    diags_.error(a.span, "@renamed takes the old field name as a string");
                    continue;
                }
                renamed = std::string(static_cast<StringLitExpr*>(a.args[0].value)->value);
            }
            if (!renamed.empty() && info.kind != ir::TypeKind::State) {
                diags_.error(f.span, "@renamed only applies to state fields");
            }
            fields.push_back({std::string(str(f.name)), ft, renamed});
            if (isHandle) {
                auto* getter = prog_->fnStorage.emplace_back(std::make_unique<FnInfo>()).get();
                getter->name = std::format("{}.{}", str(name), str(f.name));
                getter->isHost = true;
                getter->owner = entry.type;
                getter->hasSelf = true;
                getter->params = {entry.type};
                getter->result = ft;
                getter->index = static_cast<std::uint32_t>(prog_->imports.size());
                prog_->imports.push_back(getter);
                properties_[{entry.type, f.name.id}] = {getter->index, ft};
            }
        }
        for (const Attribute& a : entry.decl->attrs) {
            if (str(a.name) != "awaitable") {
                continue;
            }
            if (!isHandle) {
                diags_.error(a.span, "@awaitable needs a @handle type");
                continue;
            }
            // Attribute arguments are expressions: `[T]`, `(A, B)`, `T?` (a propagation), `Map::<K, V>` (spec 3.6).
            auto typeOf = [&](this auto& self, Expr* v) -> TypeId {
                switch (v->kind) {
                case ExprKind::Ident: {
                    auto* id = static_cast<IdentExpr*>(v);
                    if (id->typeArgs.empty()) {
                        return lookupTypeName(id->name, v->span, true);
                    }
                    NamedTypeExpr named{};
                    named.kind = TypeExprKind::Named;
                    named.span = v->span;
                    named.path = std::span<Symbol>(&id->name, 1);
                    named.args = id->typeArgs;
                    return resolveType(&named);
                }
                case ExprKind::ListLit: {
                    auto* list = static_cast<ListLitExpr*>(v);
                    if (list->elems.size() == 1) {
                        TypeId elem = self(list->elems[0]);
                        return elem == ir::kError ? elem : types_->list(elem);
                    }
                    break;
                }
                case ExprKind::TupleLit: {
                    std::vector<TypeId> elems;
                    for (Expr* e : static_cast<TupleLitExpr*>(v)->elems) {
                        elems.push_back(self(e));
                        if (elems.back() == ir::kError) {
                            return ir::kError;
                        }
                    }
                    return types_->tuple(elems);
                }
                case ExprKind::Propagate: {
                    TypeId inner = self(static_cast<PropagateExpr*>(v)->inner);
                    return inner == ir::kError || types_->kind(inner) == ir::TypeKind::Nullable
                               ? ir::kError
                               : types_->nullable(inner);
                }
                default:
                    break;
                }
                diags_.error(v->span, "@awaitable takes the type that 'wait' produces");
                return ir::kError;
            };
            TypeId produced = a.args.empty() ? ir::kVoid : typeOf(a.args[0].value);
            if (produced != ir::kVoid && produced != ir::kError && !types_->hostBuildable(produced)) {
                diags_.error(a.span, "an awaitable cannot produce a state or a function value");
            }
            awaitable_[entry.type] = produced;
        }
        for (const Attribute& a : entry.decl->attrs) {
            std::string_view an = str(a.name);
            if (an == "version") {
                if (info.kind != ir::TypeKind::State || a.args.size() != 1 || a.args[0].value->kind != ExprKind::IntLit) {
                    diags_.error(a.span, "@version(n) goes on a state with an integer");
                    continue;
                }
                types_->mutableInfo(entry.type).version =
                    static_cast<std::uint32_t>(static_cast<IntLitExpr*>(a.args[0].value)->value);
            } else if (an == "persist") {
                if (info.kind != ir::TypeKind::State) {
                    diags_.error(a.span, "@persist goes on a state");
                    continue;
                }
                types_->mutableInfo(entry.type).persisted = true;
                for (const AttrArg& arg : a.args) {
                    if (!arg.name.valid() || str(arg.name) != "per" || arg.value->kind != ExprKind::Ident) {
                        diags_.error(a.span, "@persist takes only 'per = HandleType'");
                        continue;
                    }
                    TypeId per = lookupTypeName(static_cast<IdentExpr*>(arg.value)->name, arg.value->span, true);
                    if (per != ir::kError && types_->kind(per) != ir::TypeKind::Handle) {
                        diags_.error(arg.value->span, "'per' needs a host handle type");
                        continue;
                    }
                    types_->mutableInfo(entry.type).persistPer = per;
                }
            } else if (an == "repr") {
                bool c = a.args.size() == 1 && a.args[0].value->kind == ExprKind::Ident &&
                         str(static_cast<IdentExpr*>(a.args[0].value)->name) == "C";
                if (!c || info.kind != ir::TypeKind::Struct || !entry.decl->typeParams.empty()) {
                    diags_.error(a.span, "@repr(C) goes on a non-generic struct");
                    continue;
                }
                types_->mutableInfo(entry.type).reprC = true;
            } else if (an != "handle" && an != "awaitable" && an != "derive") {
                diags_.warning(a.span, std::format("unknown attribute '@{}'", an));
            }
        }
        ir::TypeInfo& done = types_->mutableInfo(entry.type);
        if (!isHandle) {
            done.fields = std::move(fields);
        }
        done.complete = true;
    }
    types_->refreshInstances();
    for (auto& entryPtr : typeEntries_) {
        if (entryPtr->decl == nullptr || !types_->info(entryPtr->type).reprC) {
            continue;
        }
        for (const FieldDecl& fd : entryPtr->decl->fields) {
            if (fd.type == nullptr || fd.type->kind != TypeExprKind::Named) {
                continue;
            }
            auto* named = static_cast<NamedTypeExpr*>(fd.type);
            std::string_view n = named->path.size() == 1 ? str(named->path[0]) : std::string_view{};
            if (n == "int" || n == "float") {
                diags_.error(fd.span, std::format("'{}' has no fixed C width; write {} in a @repr(C) field", n,
                                                  n == "int" ? "i64 or i32" : "f64 or f32"));
            }
        }
        for (const ir::Field& f : types_->info(entryPtr->type).fields) {
            bool ok = types_->isScalar(f.type) ||
                      (types_->kind(f.type) == ir::TypeKind::Struct && types_->info(f.type).reprC);
            if (!ok) {
                diags_.error(entryPtr->decl->span, std::format("@repr(C) field '{}' must be a number, bool, char or "
                                                               "another @repr(C) struct",
                                                               f.name));
            }
        }
    }
    // A struct that contains itself by value has no finite size.
    for (auto& entryPtr : typeEntries_) {
        TypeEntry& entry = *entryPtr;
        Symbol name = entry.decl != nullptr ? entry.decl->name : Symbol{};
        if (entry.decl == nullptr || types_->kind(entry.type) != ir::TypeKind::Struct) {
            continue;
        }
        std::vector<TypeId> stack{entry.type};
        std::vector<TypeId> seen;
        while (!stack.empty()) {
            TypeId t = stack.back();
            stack.pop_back();
            for (const ir::Field& f : types_->info(t).fields) {
                if (f.type == entry.type) {
                    diags_.error(entry.decl->span, std::format("struct '{}' contains itself", str(name)));
                    stack.clear();
                    break;
                }
                if (types_->kind(f.type) == ir::TypeKind::Struct &&
                    std::find(seen.begin(), seen.end(), f.type) == seen.end()) {
                    seen.push_back(f.type);
                    stack.push_back(f.type);
                }
            }
        }
    }
}

TypeId Checker::lookupTypeName(Symbol name, Span span, bool report) {
    static const std::pair<std::string_view, TypeId> builtins[] = {
        {"void", ir::kVoid}, {"bool", ir::kBool}, {"i8", ir::kI8},   {"i16", ir::kI16},   {"i32", ir::kI32},
        {"i64", ir::kI64},   {"u8", ir::kU8},     {"u16", ir::kU16}, {"u32", ir::kU32},   {"u64", ir::kU64},
        {"f32", ir::kF32},   {"f64", ir::kF64},   {"char", ir::kChar}, {"str", ir::kStr}, {"Bytes", ir::kBytes},
        {"int", ir::kI64},   {"float", ir::kF64}, {"byte", ir::kU8},
    };
    std::string_view text = str(name);
    for (const auto& [n, id] : builtins) {
        if (n == text) {
            return id;
        }
    }
    if (text == "Self" && selfType_ != ir::kError) {
        return selfType_;
    }
    for (auto it = typeScopes_.rbegin(); it != typeScopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) {
            return found->second;
        }
    }
    if (TypeEntry* e = findType(name)) {
        return e->type;
    }
    if (report) {
        diags_.error(span, std::format("unknown type '{}'", text));
    }
    return ir::kError;
}

TypeId Checker::resolveType(TypeExpr* t) {
    if (t == nullptr) {
        return ir::kError;
    }
    TypeId r = resolveTypeExpr(t);
    t->type = r;
    return r;
}

TypeId Checker::resolveTypeExpr(TypeExpr* t) {
    switch (t->kind) {
    case TypeExprKind::Named: {
        auto* n = static_cast<NamedTypeExpr*>(t);
        if (n->path.empty()) {
            return ir::kError;
        }
        Symbol last = n->path[n->path.size() - 1];
        std::string_view text = str(last);
        if (text == "Result") {
            if (n->args.size() != 2) {
                diags_.error(t->span, "'Result' takes 2 type arguments");
                return ir::kError;
            }
            return types_->result(resolveType(n->args[0]), resolveType(n->args[1]));
        }
        if (text == "List" || text == "Map") {
            std::size_t want = text == "List" ? 1 : 2;
            if (n->args.size() != want) {
                diags_.error(t->span, std::format("'{}' takes {} type argument(s)", text, want));
                return ir::kError;
            }
            TypeId a = resolveType(n->args[0]);
            if (want == 1) {
                return types_->list(a);
            }
            TypeId v = resolveType(n->args[1]);
            checkMapKey(a, n->args[0]->span);
            return types_->map(a, v);
        }
        TypeId found = ir::kError;
        if (n->path.size() > 1) {
            std::string pkg;
            if (!qualifier(n->path.first(n->path.size() - 1), pkg)) {
                diags_.error(t->span, std::format("unknown package '{}'", pkg));
                return ir::kError;
            }
            auto* te = static_cast<TypeEntry*>(findIn(pkg, last, NameKind::Type));
            if (te == nullptr) {
                diags_.error(t->span, std::format("package '{}' has no public type '{}'", pkg, text));
                return ir::kError;
            }
            found = te->type;
        } else {
            found = lookupTypeName(last, t->span, true);
        }
        if (found != ir::kError && types_->isGenericBase(found)) {
            const ir::TypeInfo& base = types_->info(found);
            if (n->args.size() != base.typeParams.size()) {
                diags_.error(t->span, std::format("'{}' takes {} type argument(s)", text, base.typeParams.size()));
                return ir::kError;
            }
            std::vector<TypeId> args;
            for (TypeExpr* a : n->args) {
                args.push_back(resolveType(a));
            }
            TypeId inst = types_->instantiate(found, args);
            checkTypeArgs(inst, t->span);
            return inst;
        }
        if (!n->args.empty()) {
            diags_.error(t->span, std::format("type '{}' takes no type arguments", text));
        }
        return found;
    }
    case TypeExprKind::List: return types_->list(resolveType(static_cast<ListTypeExpr*>(t)->elem));
    case TypeExprKind::Array:
        diags_.error(t->span, "fixed-size arrays are not supported yet; use a list");
        return types_->list(resolveType(static_cast<ArrayTypeExpr*>(t)->elem));
    case TypeExprKind::Tuple: {
        std::vector<TypeId> elems;
        for (TypeExpr* e : static_cast<TupleTypeExpr*>(t)->elems) {
            elems.push_back(resolveType(e));
        }
        if (elems.empty()) {
            return ir::kVoid;
        }
        return types_->tuple(elems);
    }
    case TypeExprKind::Nullable: {
        TypeId inner = resolveType(static_cast<NullableTypeExpr*>(t)->inner);
        if (inner == ir::kVoid) {
            diags_.error(t->span, "'void' cannot be nullable");
            return ir::kError;
        }
        return types_->nullable(inner);
    }
    case TypeExprKind::Fn: {
        auto* f = static_cast<FnTypeExpr*>(t);
        std::vector<TypeId> params;
        bool ok = true;
        for (TypeExpr* p : f->params) {
            params.push_back(resolveType(p));
            ok = ok && params.back() != ir::kError && params.back() != ir::kVoid;
        }
        TypeId result = f->result != nullptr ? resolveType(f->result) : ir::kVoid;
        return ok && result != ir::kError ? types_->fn(params, result) : ir::kError;
    }
    }
    return ir::kError;
}

FnInfo* Checker::declareFn(FnDecl* fn, TypeId owner, bool inInterface, const ImplParams& impl) {
    auto* info = prog_->fnStorage.emplace_back(std::make_unique<FnInfo>()).get();
    info->decl = fn;
    fn->info = info;
    info->owner = owner;
    info->hasSelf = fn->hasSelf;
    info->isSuspend = fn->isSuspend;
    info->isHost = fn->body == nullptr;
    info->typeParams = impl.ids;
    info->typeParamNames = impl.names;
    info->bounds = impl.bounds;
    pushTypeParams(fn->typeParams, info->typeParams, info->typeParamNames, info->bounds);
    struct PopScope {
        std::vector<std::unordered_map<Symbol, TypeId>>& s;
        ~PopScope() { s.pop_back(); }
    } pop{typeScopes_};
    if (info->generic() && info->isHost) {
        diags_.error(fn->span, "host functions cannot be generic");
    }
    if (info->isHost && !inInterface) {
        diags_.error(fn->span, "a function without a body can only be declared in a host interface (.qzi)");
    }
    if (!info->isHost && inInterface) {
        diags_.error(fn->span, "functions in a host interface (.qzi) cannot have a body");
    }
    TypeId savedSelf = selfType_;
    selfType_ = owner;
    if (owner != ir::kError) {
        info->name = std::format("{}.{}", typeName(owner), str(fn->name));
        if (fn->hasSelf) {
            info->params.push_back(owner);
            info->paramNames.push_back(names_.intern("self"));
            info->defaults.push_back(nullptr);
        }
    } else {
        const std::string& pkg = files_[fn->span.file].package;
        info->name = info->isHost && !pkg.empty() ? pkg + "." + std::string(str(fn->name)) : std::string(str(fn->name));
        if (fn->hasSelf) {
            diags_.error(fn->span, "'self' is only allowed in impl blocks");
            info->hasSelf = false;
        }
    }
    for (Param& p : fn->params) {
        info->params.push_back(resolveType(p.type));
        info->paramNames.push_back(p.name);
        info->defaults.push_back(p.defaultValue);
    }
    info->result = fn->result != nullptr ? resolveType(fn->result) : ir::kVoid;
    selfType_ = savedSelf;
    for (const Attribute& a : fn->attrs) {
        if (str(a.name) != "deprecated") {
            continue;
        }
        info->deprecated = true;
        if (a.args.size() == 1 && a.args[0].value->kind == ExprKind::StringLit) {
            info->deprecation = static_cast<StringLitExpr*>(a.args[0].value)->value;
        } else if (!a.args.empty()) {
            diags_.error(a.span, "@deprecated takes one string: what to use instead");
        }
    }
    if (hasAttr(fn->attrs, "property")) {
        if (!info->isHost || !fn->hasSelf || fn->params.size() != 0 || info->result == ir::kVoid) {
            diags_.error(fn->span, "@property needs a host method with only 'self' that returns a value");
        } else {
            info->isProperty = true;
        }
    }
    if (info->isHost) {
        info->index = static_cast<std::uint32_t>(prog_->imports.size());
        prog_->imports.push_back(info);
    } else if (info->generic()) {
        prog_->generics.push_back(info);
    } else {
        info->index = static_cast<std::uint32_t>(prog_->functions.size());
        prog_->functions.push_back(info);
    }
    if (owner != ir::kError) {
        const ir::TypeInfo& oinfo = types_->info(owner);
        bool genericOwner = oinfo.genericBase != 0 && types_->hasParams(owner);
        TypeId key0 = genericOwner ? oinfo.genericBase : owner;
        auto key = std::make_pair(key0, fn->name.id);
        auto& table = genericOwner ? genericMethods_ : methods_;
        if (table.contains(key) || properties_.contains(key)) {
            diags_.error(fn->span, std::format("'{}' is already declared", info->name));
        }
        table[key] = info;
        prog_->methodTable[{key0, std::string(str(fn->name))}] = info;
    } else {
        addName(fn->name, NameKind::Function, info, fn, fn->span.file);
    }
    return info;
}

void Checker::declareFunctions() {
    for (Module* mod : modules_) {
        currentFile_ = mod->file->id();
        bool iface = mod->file->isInterface();
        for (Decl* d : mod->decls) {
            if (d->kind == DeclKind::Fn) {
                declareFn(static_cast<FnDecl*>(d), ir::kError, iface);
            } else if (d->kind == DeclKind::Impl) {
                auto* impl = static_cast<ImplDecl*>(d);
                ImplParams ip;
                pushTypeParams(impl->typeParams, ip.ids, ip.names, ip.bounds);
                TypeId owner = resolveType(impl->target);
                if (owner == ir::kError) {
                    typeScopes_.pop_back();
                    continue;
                }
                const ir::TypeInfo& oinfo = types_->info(owner);
                if (!ip.ids.empty() && (oinfo.typeArgs != ip.ids)) {
                    diags_.error(impl->span, "a generic impl must target its type with exactly its parameters, in order");
                } else {
                    mergeTypeBounds(owner, ip);
                }
                std::vector<FnInfo*> methods;
                for (FnDecl* m : impl->methods) {
                    methods.push_back(declareFn(m, owner, iface, ip));
                }
                typeScopes_.pop_back();
                declareImplTrait(impl, owner, methods, ip);
            } else if (d->kind == DeclKind::Const) {
                auto* c = static_cast<ConstDecl*>(d);
                auto* g = prog_->globalStorage.emplace_back(std::make_unique<GlobalInfo>()).get();
                g->decl = c;
                c->info = g;
                addName(c->name, NameKind::Global, g, c, currentFile_);
                globalList_.push_back(g);
            }
        }
    }
}

std::unique_ptr<Program> Checker::run() {
    indexFiles();
    declareTypes();
    completeTypes();
    declareTraits();
    resolveTypeBounds();
    declareDerives();
    declareFunctions();
    functionsDeclared_ = true;
    checkDerives();
    flushPendingChecks();
    checkFunctionTypes();
    for (GlobalInfo* g : globalList_) {
        currentFile_ = g->decl->span.file;
        if (g->type == ir::kError && !g->checking) {
            g->checking = true;
            TypeId declared = g->decl->type != nullptr ? resolveType(g->decl->type) : ir::kError;
            g->type = checkValue(g->decl->value, declared);
            if (declared != ir::kError) {
                expectAssignable(g->type, declared, g->decl->value->span);
                g->type = declared;
            }
        }
    }
    for (auto& [type, decl] : prog_->structDecls) {
        currentFile_ = decl->span.file;
        selfType_ = type;
        for (FieldDecl& f : decl->fields) {
            if (f.defaultValue != nullptr) {
                TypeId ft = ir::kError;
                for (const ir::Field& field : types_->info(type).fields) {
                    if (field.name == str(f.name)) {
                        ft = field.type;
                    }
                }
                expectAssignable(checkValue(f.defaultValue, ft), ft, f.defaultValue->span);
            }
        }
        selfType_ = ir::kError;
    }
    collectFixedSignatures();
    for (std::size_t i = 0; i < prog_->functions.size(); ++i) {
        checkFunction(prog_->functions[i]);
    }
    for (std::size_t i = 0; i < prog_->generics.size(); ++i) {
        checkFunction(prog_->generics[i]);
    }
    lint();
    return std::move(prog_);
}

std::unique_ptr<Program> analyze(std::span<Module*> modules, Interner& names, Arena& arena, Diagnostics& diags) {
    return Checker(modules, names, arena, diags).run();
}

} // namespace quartz::sema
