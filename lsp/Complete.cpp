#include "Builtins.hpp"
#include "Locate.hpp"
#include "Query.hpp"

#include <map>
#include <set>

namespace quartz::lsp {

using namespace syntax;

namespace {

enum ItemKind : int {
    kMethod = 2,
    kFunction = 3,
    kField = 5,
    kVariable = 6,
    kInterface = 8,
    kProperty = 10,
    kEnum = 13,
    kKeyword = 14,
    kEnumMember = 20,
    kConstant = 21,
    kStruct = 22,
};

bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

std::string name(const Analysis& a, Symbol s) { return s.valid() ? std::string(a.names().name(s)) : std::string{}; }

std::string joinPath(const Analysis& a, std::span<const Symbol> path) {
    std::string out;
    for (std::size_t i = 0; i < path.size(); ++i) {
        out += (i ? "." : "") + name(a, path[i]);
    }
    return out;
}

class Collector {
public:
    explicit Collector(const Analysis& a) : a_(a) {}

    void add(std::string label, int kind, std::string detail = {}) {
        if (label.empty() || !seen_.insert(label).second) {
            return;
        }
        items_.push_back({std::move(label), kind, std::move(detail)});
    }

    void local(const sema::Local* l, Symbol fallback) {
        if (l != nullptr) {
            add(name(a_, l->name), kVariable, a_.typeName(l->type));
        } else {
            add(name(a_, fallback), kVariable);
        }
    }

    void patternBindings(const Pattern* p) {
        if (p == nullptr) {
            return;
        }
        if (p->kind == PatternKind::Binding) {
            local(p->local, p->name);
        }
        for (const Pattern* s : p->sub) {
            patternBindings(s);
        }
    }

    std::vector<Completion> take() { return std::move(items_); }

private:
    const Analysis& a_;
    std::set<std::string> seen_;
    std::vector<Completion> items_;
};

void scopeNames(const Analysis& a, const std::vector<Node>& path, std::uint32_t offset, Collector& out) {
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const Node& n = *it;
        if (n.kind == NodeKind::Expr) {
            const auto* e = static_cast<const Expr*>(n.ptr);
            if (e->kind == ExprKind::Block) {
                const auto* b = static_cast<const BlockExpr*>(e);
                for (auto s = b->stmts.rbegin(); s != b->stmts.rend(); ++s) {
                    if ((*s)->kind == StmtKind::Let && (*s)->span.end <= offset) {
                        const auto* l = static_cast<const LetStmt*>(*s);
                        out.local(l->local, l->name);
                    }
                }
            } else if (e->kind == ExprKind::Lambda) {
                for (const Param& p : static_cast<const LambdaExpr*>(e)->params) {
                    out.local(p.local, p.name);
                }
            } else if (e->kind == ExprKind::If) {
                const auto* i = static_cast<const IfExpr*>(e);
                if (i->bindName.valid() && i->then != nullptr && i->then->span.begin <= offset &&
                    offset <= i->then->span.end) {
                    out.local(i->bindLocal, i->bindName);
                }
            }
        } else if (n.kind == NodeKind::MatchArm) {
            out.patternBindings(static_cast<const MatchArm*>(n.ptr)->pattern);
        } else if (n.kind == NodeKind::Stmt && static_cast<const Stmt*>(n.ptr)->kind == StmtKind::For) {
            const auto* f = static_cast<const ForStmt*>(n.ptr);
            if (f->body != nullptr && f->body->span.begin <= offset) {
                out.local(f->local, f->name);
                if (f->indexName.valid()) {
                    out.local(f->indexLocal, f->indexName);
                }
            }
        } else if (n.kind == NodeKind::Decl && static_cast<const Decl*>(n.ptr)->kind == DeclKind::Fn) {
            const auto* fn = static_cast<const FnDecl*>(n.ptr);
            if (fn->hasSelf) {
                out.add("self", kVariable, fn->info != nullptr ? a.typeName(fn->info->owner) : std::string{});
            }
            for (const Param& p : fn->params) {
                out.local(p.local, p.name);
            }
        }
    }
}

void declItems(const Analysis& a, const Module& m, bool qualifiedOnly, std::set<std::string>* only, Collector& out) {
    for (const Decl* d : m.decls) {
        if (d == nullptr || !d->name.valid() || (qualifiedOnly && d->visibility != Visibility::Pub)) {
            continue;
        }
        std::string n = name(a, d->name);
        if (only != nullptr && !only->contains(n)) {
            continue;
        }
        switch (d->kind) {
        case DeclKind::Fn: {
            const auto* fn = static_cast<const FnDecl*>(d);
            out.add(n, kFunction, fn->info != nullptr ? signature(a, *fn->info) : std::string{});
            break;
        }
        case DeclKind::Struct:
        case DeclKind::State:
            out.add(n, kStruct);
            break;
        case DeclKind::Enum:
            out.add(n, kEnum);
            break;
        case DeclKind::Trait:
            out.add(n, kInterface);
            break;
        case DeclKind::Const: {
            const auto* c = static_cast<const ConstDecl*>(d);
            out.add(n, kConstant, c->info != nullptr ? a.typeName(c->info->type) : std::string{});
            break;
        }
        case DeclKind::Impl:
            break;
        }
    }
}

void topLevelNames(const Analysis& a, const Module& current, Collector& out) {
    std::string package = joinPath(a, current.package);
    std::set<std::string> starPackages;
    std::map<std::string, std::set<std::string>> named;
    for (const Import& imp : current.imports) {
        std::string path = joinPath(a, imp.path);
        if (imp.star) {
            starPackages.insert(path);
        }
        for (Symbol s : imp.names) {
            named[path].insert(name(a, s));
        }
        if (!imp.star && imp.names.empty() && imp.path.size() >= 2) {
            starPackages.insert(path);
            named[joinPath(a, imp.path.subspan(0, imp.path.size() - 1))].insert(name(a, imp.path.back()));
        }
    }
    for (const Module* m : a.modules()) {
        if (m == nullptr) {
            continue;
        }
        std::string pkg = joinPath(a, m->package);
        bool iface = m->file != nullptr && m->file->isInterface();
        if (m == &current || pkg == package || (iface && pkg.empty()) || starPackages.contains(pkg)) {
            declItems(a, *m, m != &current && pkg != package && !(iface && pkg.empty()), nullptr, out);
        } else if (auto it = named.find(pkg); it != named.end()) {
            declItems(a, *m, true, &it->second, out);
        }
    }
}

void memberItems(const Analysis& a, std::uint32_t t, bool staticOnly, Collector& out) {
    const sema::Program* p = a.program();
    if (p == nullptr || t == 0 || t >= p->types.size()) {
        return;
    }
    const ir::TypeInfo& info = p->types.info(t);
    if (staticOnly) {
        for (const ir::VariantInfo& v : info.variants) {
            out.add(v.name, kEnumMember);
        }
    } else {
        for (const ir::Field& f : info.fields) {
            out.add(f.name, info.kind == ir::TypeKind::Handle ? kProperty : kField, a.typeName(f.type));
        }
        if (info.kind == ir::TypeKind::Tuple) {
            for (std::size_t i = 0; i < info.elems.size(); ++i) {
                out.add(std::to_string(i), kField, a.typeName(info.elems[i]));
            }
        }
    }
    std::uint32_t base = info.genericBase != 0 ? info.genericBase : t;
    for (const auto& [key, fn] : p->methodTable) {
        if ((key.first == t || key.first == base) && fn != nullptr && fn->hasSelf != staticOnly) {
            out.add(key.second, fn->isProperty ? kProperty : staticOnly ? kFunction : kMethod, signature(a, *fn));
        }
    }
    if (!staticOnly) {
        for (const BuiltinMethod& m : builtinMethods(a, t)) {
            out.add(m.name, kMethod, m.signature);
        }
    }
}

const Expr* receiverAt(const std::vector<Node>& path, std::uint32_t dot) {
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        if (it->kind != NodeKind::Expr) {
            continue;
        }
        const auto* e = static_cast<const Expr*>(it->ptr);
        if (e->kind == ExprKind::Member && e->span.begin < dot) {
            const Expr* obj = static_cast<const MemberExpr*>(e)->object;
            if (obj != nullptr && obj->span.end <= dot) {
                return obj;
            }
        }
        if (e->span.end == dot) {
            return e;
        }
    }
    return nullptr;
}

} // namespace

std::vector<Completion> completeAt(const Analysis& a, std::uint32_t file, std::uint32_t offset) {
    Collector out(a);
    const Module* m = a.module(file);
    std::string_view text = a.input(file).text;
    if (m == nullptr || offset > text.size()) {
        return {};
    }
    std::uint32_t start = offset;
    while (start > 0 && isIdentChar(text[start - 1])) {
        --start;
    }
    if (start > 0 && text[start - 1] == '.' && !(start > 1 && text[start - 2] == '.')) {
        std::uint32_t dot = start - 1;
        bool safe = dot > 0 && text[dot - 1] == '?';
        std::uint32_t objEnd = safe ? dot - 1 : dot;
        while (objEnd > 0 && (text[objEnd - 1] == ' ' || text[objEnd - 1] == '\t')) {
            --objEnd;
        }
        const Expr* recv = receiverAt(locate(*m, offset, true), objEnd);
        if (recv == nullptr) {
            recv = receiverAt(locate(*m, objEnd > 0 ? objEnd - 1 : 0, true), objEnd);
        }
        if (recv == nullptr) {
            return {};
        }
        std::uint32_t t = recv->type;
        if (recv->kind == ExprKind::Ident && static_cast<const IdentExpr*>(recv)->ref != IdentRef::Local &&
            static_cast<const IdentExpr*>(recv)->ref != IdentRef::Const) {
            if (std::uint32_t named = typeNamed(a, name(a, static_cast<const IdentExpr*>(recv)->name))) {
                memberItems(a, named, true, out);
                return out.take();
            }
        }
        const sema::Program* p = a.program();
        if (p != nullptr && t != 0 && t < p->types.size() && p->types.kind(t) == ir::TypeKind::Nullable && safe) {
            t = p->types.info(t).elem;
        }
        memberItems(a, t, false, out);
        return out.take();
    }
    scopeNames(a, locate(*m, offset, true), offset, out);
    topLevelNames(a, *m, out);
    for (const BuiltinFunction& f : builtinFunctions()) {
        out.add(std::string(f.name), kFunction, std::string(f.signature));
    }
    for (std::string_view t : builtinTypes()) {
        out.add(std::string(t), kStruct);
    }
    for (std::string_view k : keywords()) {
        out.add(std::string(k), kKeyword);
    }
    return out.take();
}

} // namespace quartz::lsp
