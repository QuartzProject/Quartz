#include "Query.hpp"

#include "Builtins.hpp"
#include "Locate.hpp"

#include <format>

namespace quartz::lsp {

using namespace syntax;

namespace {

std::string_view sourceText(const Analysis& a, Span s) {
    if (s.file >= a.fileCount()) {
        return {};
    }
    std::string_view text = a.input(s.file).text;
    if (s.begin > s.end || s.end > text.size()) {
        return {};
    }
    return text.substr(s.begin, s.end - s.begin);
}

std::string name(const Analysis& a, Symbol s) { return s.valid() ? std::string(a.names().name(s)) : "?"; }

std::string localHover(const Analysis& a, const sema::Local* l, std::string_view kind) {
    return std::format("{} {}: {}", kind, name(a, l->name), a.typeName(l->type));
}

std::string typeHover(const Analysis& a, std::uint32_t t) {
    const sema::Program* p = a.program();
    if (p == nullptr || t >= p->types.size()) {
        return {};
    }
    const ir::TypeInfo& info = p->types.info(t);
    std::string out;
    switch (info.kind) {
    case ir::TypeKind::Struct:
    case ir::TypeKind::State:
    case ir::TypeKind::Handle: {
        const char* kw = info.kind == ir::TypeKind::State ? "state" : "struct";
        out = std::format("{}{} {} {{", info.kind == ir::TypeKind::Handle ? "@handle\n" : "", kw, a.typeName(t));
        for (const ir::Field& f : info.fields) {
            out += std::format("\n    {}: {}", f.name, a.typeName(f.type));
        }
        out += info.fields.empty() ? "}" : "\n}";
        return out;
    }
    case ir::TypeKind::Enum: {
        if (p->types.isResult(t)) {
            return a.typeName(t);
        }
        out = std::format("enum {} {{", a.typeName(t));
        for (const ir::VariantInfo& v : info.variants) {
            out += "\n    " + v.name;
            if (!v.payload.empty()) {
                out += "(";
                for (std::size_t i = 0; i < v.payload.size(); ++i) {
                    out += (i ? ", " : "") + a.typeName(v.payload[i]);
                }
                out += ")";
            }
        }
        out += info.variants.empty() ? "}" : "\n}";
        return out;
    }
    default:
        return a.typeName(t);
    }
}

std::string variantHover(const Analysis& a, std::uint32_t t, std::uint32_t variant) {
    const ir::TypeInfo& info = a.program()->types.info(t);
    if (variant >= info.variants.size()) {
        return a.typeName(t);
    }
    const ir::VariantInfo& v = info.variants[variant];
    std::string out = std::format("{}.{}", a.typeName(t), v.name);
    if (!v.payload.empty()) {
        out += "(";
        for (std::size_t i = 0; i < v.payload.size(); ++i) {
            out += (i ? ", " : "") + a.typeName(v.payload[i]);
        }
        out += ")";
    }
    return out;
}

const Decl* traitDecl(const Analysis& a, Symbol name) {
    for (const Module* m : a.modules()) {
        for (const Decl* d : m != nullptr ? m->decls : std::span<Decl*>{}) {
            if (d != nullptr && d->kind == DeclKind::Trait && d->name == name) {
                return d;
            }
        }
    }
    return nullptr;
}

std::optional<Span> typeDeclSpan(const Analysis& a, std::uint32_t t) {
    if (const Decl* d = typeDecl(a, t)) {
        return d->nameSpan;
    }
    return std::nullopt;
}

Resolved typeTarget(const Analysis& a, std::uint32_t t, Span range) {
    return {typeHover(a, t), range, typeDeclSpan(a, t)};
}

std::optional<Resolved> fnTarget(const Analysis& a, const sema::FnInfo* fn, Span range) {
    if (fn == nullptr) {
        return std::nullopt;
    }
    Resolved r{signature(a, *fn), range, std::nullopt};
    if (fn->decl != nullptr) {
        r.declaration = fn->decl->nameSpan;
    }
    return r;
}

std::uint32_t unwrapNullable(const Analysis& a, std::uint32_t t) {
    const sema::Program* p = a.program();
    if (p != nullptr && t < p->types.size() && p->types.kind(t) == ir::TypeKind::Nullable) {
        return p->types.info(t).elem;
    }
    return t;
}

// Span of the member name at the end of `a.b`.
Span memberNameSpan(const Analysis& a, const MemberExpr* m) {
    Span s = m->span;
    std::size_t len = m->name.valid() ? a.names().name(m->name).size() : 0;
    if (s.end >= len && s.end - len >= s.begin) {
        s.begin = static_cast<std::uint32_t>(s.end - len);
    }
    return s;
}

std::optional<Resolved> resolveMember(const Analysis& a, const MemberExpr* m, const Expr* parent) {
    const sema::Program* p = a.program();
    Span range = memberNameSpan(a, m);
    std::string member = name(a, m->name);
    if (parent != nullptr && parent->kind == ExprKind::Call && static_cast<const CallExpr*>(parent)->callee == m) {
        const auto* call = static_cast<const CallExpr*>(parent);
        if (call->fn != nullptr) {
            return fnTarget(a, call->fn, range);
        }
        std::uint32_t recv = unwrapNullable(a, m->object != nullptr ? m->object->type : 0);
        if (call->callKind == CallKind::Variant) {
            if (const auto* e = enumDeclOf(a, call->type)) {
                if (call->variant < e->variants.size()) {
                    return Resolved{variantHover(a, call->type, call->variant), range, e->variants[call->variant].span};
                }
            }
        }
        if (const BuiltinFunction* b = findBuiltinFunction(member);
            b != nullptr && !b->package.empty() && call->callKind == CallKind::Builtin && recv == 0) {
            return Resolved{std::format("fn {}", b->signature), range, std::nullopt};
        }
        if (auto b = findBuiltinMethod(a, recv, member)) {
            return Resolved{std::format("fn {}.{}", a.typeName(recv), b->signature), range, std::nullopt};
        }
        return std::nullopt;
    }
    if (m->isVariant) {
        if (const EnumDecl* e = enumDeclOf(a, m->type)) {
            if (m->fieldIndex < e->variants.size()) {
                return Resolved{variantHover(a, m->type, m->fieldIndex), range, e->variants[m->fieldIndex].span};
            }
        }
        return std::nullopt;
    }
    if (m->object == nullptr || p == nullptr) {
        return std::nullopt;
    }
    std::uint32_t owner = unwrapNullable(a, m->object->type);
    if (owner == 0 || owner >= p->types.size()) {
        return std::nullopt;
    }
    if (m->isProperty && m->getter < p->imports.size() && p->imports[m->getter]->decl != nullptr) {
        return fnTarget(a, p->imports[m->getter], range);
    }
    Resolved r{std::format("{}.{}: {}", a.typeName(owner), member, a.typeName(m->type)), range, std::nullopt};
    if (m->type == 0) {
        return std::nullopt;
    }
    if (const Decl* d = typeDecl(a, owner); d != nullptr && d->kind != DeclKind::Enum) {
        for (const FieldDecl& f : static_cast<const StructDecl*>(d)->fields) {
            if (f.name == m->name) {
                r.declaration = f.span;
            }
        }
    }
    return r;
}

std::optional<Resolved> resolveIdent(const Analysis& a, const IdentExpr* id, const Expr* parent) {
    switch (id->ref) {
    case IdentRef::Local:
        if (id->local != nullptr) {
            return Resolved{localHover(a, id->local, id->local->mutable_ ? "var" : "let"), id->span, id->local->span};
        }
        break;
    case IdentRef::Function:
        return fnTarget(a, id->fn, id->span);
    case IdentRef::Global:
    case IdentRef::Const:
        if (id->global != nullptr && id->global->decl != nullptr) {
            return Resolved{std::format("const {}: {}", name(a, id->name), a.typeName(id->global->type)), id->span,
                            id->global->decl->nameSpan};
        }
        break;
    case IdentRef::Type:
        if (parent != nullptr && parent->type != 0) {
            return typeTarget(a, unwrapNullable(a, parent->type), id->span);
        }
        break;
    default:
        break;
    }
    if (id->ref == IdentRef::Unresolved || id->ref == IdentRef::Type) {
        if (std::uint32_t t = typeNamed(a, a.names().name(id->name))) {
            return typeTarget(a, t, id->span);
        }
    }
    if (const BuiltinFunction* b = findBuiltinFunction(name(a, id->name))) {
        return Resolved{std::format("fn {}", b->signature), id->span, std::nullopt};
    }
    if (id->type != 0) {
        return Resolved{a.typeName(id->type), id->span, std::nullopt};
    }
    return std::nullopt;
}

std::optional<Resolved> resolvePattern(const Analysis& a, const Pattern* p, const std::vector<Node>& path) {
    if (p->local != nullptr) {
        return Resolved{localHover(a, p->local, "let"), p->span, p->local->span};
    }
    if (p->kind != PatternKind::Variant) {
        return std::nullopt;
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        if (it->kind == NodeKind::Pattern && it->ptr != p) {
            return std::nullopt; // nested payload pattern: the enum type is not recorded
        }
        if (it->kind == NodeKind::Expr && static_cast<const Expr*>(it->ptr)->kind == ExprKind::Match) {
            const Expr* subject = static_cast<const MatchExpr*>(it->ptr)->subject;
            std::uint32_t t = unwrapNullable(a, subject != nullptr ? subject->type : 0);
            if (const EnumDecl* e = enumDeclOf(a, t); e != nullptr && p->variantIndex < e->variants.size()) {
                return Resolved{variantHover(a, t, p->variantIndex), p->span, e->variants[p->variantIndex].span};
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<Resolved> resolveDeclName(const Analysis& a, const Decl* d) {
    switch (d->kind) {
    case DeclKind::Fn: {
        const auto* fn = static_cast<const FnDecl*>(d);
        const sema::FnInfo* info = fn->info;
        for (std::size_t i = 0; info == nullptr && a.program() != nullptr && i < a.program()->fnStorage.size(); ++i) {
            if (a.program()->fnStorage[i]->decl == fn) {
                info = a.program()->fnStorage[i].get(); // trait methods are not linked from their declaration
            }
        }
        return fnTarget(a, info, d->nameSpan);
    }
    case DeclKind::Const: {
        const auto* c = static_cast<const ConstDecl*>(d);
        std::uint32_t t = c->info != nullptr ? c->info->type : 0;
        return Resolved{std::format("const {}: {}", name(a, d->name), a.typeName(t)), d->nameSpan, d->nameSpan};
    }
    case DeclKind::Trait:
        return Resolved{"trait " + name(a, d->name), d->nameSpan, d->nameSpan};
    default:
        break;
    }
    const sema::Program* p = a.program();
    if (p != nullptr) {
        for (const auto& [t, decl] : p->structDecls) {
            if (decl == d) {
                return Resolved{typeHover(a, t), d->nameSpan, d->nameSpan};
            }
        }
        for (const auto& [t, decl] : p->enumDecls) {
            if (decl == d) {
                return Resolved{typeHover(a, t), d->nameSpan, d->nameSpan};
            }
        }
    }
    return std::nullopt;
}

std::optional<Resolved> resolveNode(const Analysis& a, const std::vector<Node>& path) {
    const Node& n = path.back();
    const Node* parentNode = path.size() >= 2 ? &path[path.size() - 2] : nullptr;
    const Expr* parent = parentNode != nullptr && parentNode->kind == NodeKind::Expr
                             ? static_cast<const Expr*>(parentNode->ptr)
                             : nullptr;
    switch (n.kind) {
    case NodeKind::DeclName:
        return resolveDeclName(a, static_cast<const Decl*>(n.ptr));
    case NodeKind::Field: {
        const auto* f = static_cast<const FieldDecl*>(n.ptr);
        std::uint32_t t = f->type != nullptr ? f->type->type : 0;
        return Resolved{std::format("{}: {}", name(a, f->name), a.typeName(t)), f->span, f->span};
    }
    case NodeKind::Variant: {
        const auto* v = static_cast<const Variant*>(n.ptr);
        const auto* e = static_cast<const EnumDecl*>(n.owner);
        return Resolved{std::format("{}.{}", name(a, e->name), name(a, v->name)), v->span, v->span};
    }
    case NodeKind::Param: {
        const auto* p = static_cast<const Param*>(n.ptr);
        std::uint32_t t = p->local != nullptr ? p->local->type : p->type != nullptr ? p->type->type : 0;
        return Resolved{std::format("{}: {}", name(a, p->name), a.typeName(t)), p->span, p->span};
    }
    case NodeKind::LetName: {
        const auto* l = static_cast<const LetStmt*>(n.ptr);
        if (l->local == nullptr) {
            return std::nullopt;
        }
        return Resolved{localHover(a, l->local, l->mutable_ ? "var" : "let"), l->nameSpan, l->nameSpan};
    }
    case NodeKind::ForName: {
        const auto* f = static_cast<const ForStmt*>(n.ptr);
        if (f->local == nullptr) {
            return std::nullopt;
        }
        return Resolved{localHover(a, f->local, "let"), f->nameSpan, f->nameSpan};
    }
    case NodeKind::Pattern:
        return resolvePattern(a, static_cast<const Pattern*>(n.ptr), path);
    case NodeKind::Type: {
        const auto* t = static_cast<const TypeExpr*>(n.ptr);
        if (t->type == 0 && t->kind == TypeExprKind::Named && !static_cast<const NamedTypeExpr*>(t)->path.empty()) {
            if (const Decl* trait = traitDecl(a, static_cast<const NamedTypeExpr*>(t)->path.back())) {
                return Resolved{"trait " + name(a, trait->name), t->span, trait->nameSpan};
            }
        }
        if (t->type == 0) {
            return std::nullopt;
        }
        return typeTarget(a, t->type, t->span);
    }
    case NodeKind::Expr: {
        const auto* e = static_cast<const Expr*>(n.ptr);
        switch (e->kind) {
        case ExprKind::Ident:
            return resolveIdent(a, static_cast<const IdentExpr*>(e), parent);
        case ExprKind::Member:
            return resolveMember(a, static_cast<const MemberExpr*>(e), parent);
        case ExprKind::SelfRef: {
            const auto* s = static_cast<const SelfExpr*>(e);
            if (s->local == nullptr) {
                return std::nullopt;
            }
            return Resolved{localHover(a, s->local, "let"), e->span, typeDeclSpan(a, s->local->type)};
        }
        case ExprKind::Block:
        case ExprKind::If:
        case ExprKind::Match:
        case ExprKind::Lambda:
            return std::nullopt;
        default:
            break;
        }
        if (e->type == 0) {
            return std::nullopt;
        }
        return Resolved{a.typeName(e->type), e->span, std::nullopt};
    }
    default:
        return std::nullopt;
    }
}

} // namespace

const EnumDecl* enumDeclOf(const Analysis& a, std::uint32_t t) {
    const Decl* d = typeDecl(a, t);
    return d != nullptr && d->kind == DeclKind::Enum ? static_cast<const EnumDecl*>(d) : nullptr;
}

std::uint32_t typeNamed(const Analysis& a, std::string_view n) {
    const sema::Program* p = a.program();
    if (p == nullptr) {
        return 0;
    }
    for (const auto& [t, d] : p->enumDecls) {
        if (d->name.valid() && a.names().name(d->name) == n) {
            return t;
        }
    }
    for (const auto& [t, d] : p->structDecls) {
        if (d->name.valid() && a.names().name(d->name) == n) {
            return t;
        }
    }
    return 0;
}

const Decl* typeDecl(const Analysis& a, std::uint32_t t) {
    const sema::Program* p = a.program();
    if (p == nullptr || t == 0 || t >= p->types.size()) {
        return nullptr;
    }
    const ir::TypeInfo& info = p->types.info(t);
    std::uint32_t base = info.genericBase != 0 ? info.genericBase : t;
    if (auto it = p->structDecls.find(base); it != p->structDecls.end()) {
        return it->second;
    }
    if (auto it = p->enumDecls.find(base); it != p->enumDecls.end()) {
        return it->second;
    }
    return nullptr;
}

std::string signature(const Analysis& a, const sema::FnInfo& fn) {
    std::string out = fn.isSuspend ? "suspend fn " : "fn ";
    out += fn.name.empty() && fn.decl != nullptr ? name(a, fn.decl->name) : fn.name;
    if (!fn.typeParamNames.empty() && fn.decl != nullptr && !fn.decl->typeParams.empty()) {
        out += "<";
        for (std::size_t i = 0; i < fn.decl->typeParams.size(); ++i) {
            out += (i ? ", " : "") + name(a, fn.decl->typeParams[i].name);
        }
        out += ">";
    }
    out += "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        if (i == 0 && fn.hasSelf) {
            out += "self";
            continue;
        }
        std::string pname = i < fn.paramNames.size() ? name(a, fn.paramNames[i]) : "_";
        out += pname + ": " + a.typeName(fn.params[i]);
        if (i < fn.defaults.size() && fn.defaults[i] != nullptr) {
            out += " = " + std::string(sourceText(a, fn.defaults[i]->span));
        }
    }
    out += ")";
    if (fn.result != ir::kVoid) {
        out += " -> " + a.typeName(fn.result);
    }
    return out;
}

std::optional<Resolved> resolveAt(const Analysis& a, std::uint32_t file, std::uint32_t offset) {
    const Module* m = a.module(file);
    if (m == nullptr) {
        return std::nullopt;
    }
    std::vector<Node> path = locate(*m, offset, false);
    if (path.empty()) {
        path = locate(*m, offset, true);
    }
    while (!path.empty()) {
        if (auto r = resolveNode(a, path)) {
            return r;
        }
        NodeKind k = path.back().kind;
        if (k != NodeKind::Expr || path.size() < 2 || path[path.size() - 2].kind != NodeKind::Expr) {
            return std::nullopt;
        }
        path.pop_back();
    }
    return std::nullopt;
}

} // namespace quartz::lsp
