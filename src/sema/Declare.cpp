#include "sema/SemaImpl.hpp"

#include <format>

namespace quartz::sema {

Checker::Checker(std::span<Module*> modules, Interner& names, Arena& arena, Diagnostics& diags)
    : modules_(modules), names_(names), arena_(arena), diags_(diags), prog_(std::make_unique<Program>()) {
    types_ = &prog_->types;
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
        for (Decl* d : mod->decls) {
            if (d->kind != DeclKind::Struct && d->kind != DeclKind::State && d->kind != DeclKind::Enum) {
                continue;
            }
            if (typeNames_.contains(d->name) || lookupTypeName(d->name, d->span, false) != ir::kError) {
                diags_.error(d->span, std::format("type '{}' is already declared", str(d->name)));
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
            if (d->kind == DeclKind::Enum) {
                entry.enumDecl = static_cast<EnumDecl*>(d);
            } else {
                entry.decl = static_cast<StructDecl*>(d);
                prog_->structDecls[entry.type] = entry.decl;
                if (!entry.decl->typeParams.empty()) {
                    diags_.error(d->span, "generic structs are not supported yet");
                }
            }
            typeNames_[d->name] = entry;
            if (kind == ir::TypeKind::State) {
                prog_->states.push_back(entry.type);
            }
        }
    }
}

void Checker::completeTypes() {
    for (auto& [name, entry] : typeNames_) {
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
            fields.push_back({std::string(str(f.name)), ft});
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
            TypeId produced = ir::kVoid;
            if (!a.args.empty()) {
                if (a.args[0].value->kind == ExprKind::Ident) {
                    produced = lookupTypeName(static_cast<IdentExpr*>(a.args[0].value)->name, a.args[0].value->span, true);
                } else {
                    diags_.error(a.span, "@awaitable takes the type that 'wait' produces");
                }
            }
            if (produced != ir::kVoid && produced != ir::kError && !types_->isScalar(produced) &&
                produced != ir::kStr && types_->kind(produced) != ir::TypeKind::Handle) {
                diags_.error(a.span, "an awaitable can only produce a scalar, a str or a handle");
            }
            awaitable_[entry.type] = produced;
        }
        ir::TypeInfo& done = types_->mutableInfo(entry.type);
        if (!isHandle) {
            done.fields = std::move(fields);
        }
        done.complete = true;
    }
    // A struct that contains itself by value has no finite size.
    for (auto& [name, entry] : typeNames_) {
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
    auto it = typeNames_.find(name);
    if (it != typeNames_.end()) {
        return it->second.type;
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
            ir::TypeKind kk = types_->kind(a);
            if (!types_->isInteger(a) && a != ir::kStr && a != ir::kBool && a != ir::kChar &&
                kk != ir::TypeKind::Handle && a != ir::kError) {
                diags_.error(n->args[0]->span, "map keys must be integers, bool, char, str or handles");
            }
            return types_->map(a, v);
        }
        if (!n->args.empty()) {
            diags_.error(t->span, std::format("type '{}' takes no type arguments", text));
        }
        return lookupTypeName(last, t->span, true);
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
        for (TypeExpr* p : f->params) {
            params.push_back(resolveType(p));
        }
        return types_->fn(params, f->result != nullptr ? resolveType(f->result) : ir::kVoid);
    }
    }
    return ir::kError;
}

void Checker::declareFn(FnDecl* fn, TypeId owner, bool inInterface) {
    auto* info = prog_->fnStorage.emplace_back(std::make_unique<FnInfo>()).get();
    info->decl = fn;
    fn->info = info;
    info->owner = owner;
    info->hasSelf = fn->hasSelf;
    info->isSuspend = fn->isSuspend;
    info->isHost = fn->body == nullptr;
    if (!fn->typeParams.empty()) {
        diags_.error(fn->span, "generic functions are not supported yet");
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
        info->name = std::string(str(fn->name));
        if (fn->hasSelf) {
            diags_.error(fn->span, "'self' is only allowed in impl blocks");
        }
    }
    for (Param& p : fn->params) {
        info->params.push_back(resolveType(p.type));
        info->paramNames.push_back(p.name);
        info->defaults.push_back(p.defaultValue);
    }
    info->result = fn->result != nullptr ? resolveType(fn->result) : ir::kVoid;
    selfType_ = savedSelf;
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
    } else {
        info->index = static_cast<std::uint32_t>(prog_->functions.size());
        prog_->functions.push_back(info);
    }
    if (owner != ir::kError) {
        auto key = std::make_pair(owner, fn->name.id);
        if (methods_.contains(key) || properties_.contains(key)) {
            diags_.error(fn->span, std::format("'{}' is already declared", info->name));
        }
        methods_[key] = info;
    } else {
        if (functions_.contains(fn->name) || globals_.contains(fn->name)) {
            diags_.error(fn->span, std::format("'{}' is already declared", info->name));
        }
        functions_[fn->name] = info;
    }
}

void Checker::declareFunctions() {
    for (Module* mod : modules_) {
        bool iface = mod->file->isInterface();
        for (Decl* d : mod->decls) {
            if (d->kind == DeclKind::Fn) {
                declareFn(static_cast<FnDecl*>(d), ir::kError, iface);
            } else if (d->kind == DeclKind::Impl) {
                auto* impl = static_cast<ImplDecl*>(d);
                if (impl->trait != nullptr) {
                    diags_.error(impl->span, "trait implementations are not supported yet");
                }
                TypeId owner = resolveType(impl->target);
                if (owner == ir::kError) {
                    continue;
                }
                for (FnDecl* m : impl->methods) {
                    declareFn(m, owner, iface);
                }
            } else if (d->kind == DeclKind::Const) {
                auto* c = static_cast<ConstDecl*>(d);
                auto* g = prog_->globalStorage.emplace_back(std::make_unique<GlobalInfo>()).get();
                g->decl = c;
                c->info = g;
                if (functions_.contains(c->name) || globals_.contains(c->name)) {
                    diags_.error(c->span, std::format("'{}' is already declared", str(c->name)));
                }
                globals_[c->name] = g;
            } else if (d->kind == DeclKind::Trait) {
                diags_.error(d->span, "traits are not supported yet");
            }
        }
    }
}

std::unique_ptr<Program> Checker::run() {
    declareTypes();
    completeTypes();
    declareFunctions();
    for (auto& [name, g] : globals_) {
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
    for (std::size_t i = 0; i < prog_->functions.size(); ++i) {
        checkFunction(prog_->functions[i]);
    }
    return std::move(prog_);
}

std::unique_ptr<Program> analyze(std::span<Module*> modules, Interner& names, Arena& arena, Diagnostics& diags) {
    return Checker(modules, names, arena, diags).run();
}

} // namespace quartz::sema
