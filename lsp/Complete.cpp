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
    kModule = 9,
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

class Collector {
public:
    explicit Collector(const Analysis& a) : a_(a) {}

    void add(std::string label, int kind, std::string detail = {}) {
        if (label.empty() || !seen_.insert(label).second) {
            return;
        }
        items_.push_back({std::move(label), kind, std::move(detail), {}, {}, {}});
    }

    // A symbol of another package; a visible name with the same label wins over it.
    void addFrom(Completion item) {
        if (seen_.contains(item.label) || !seenImports_.insert(item.package + "." + item.label).second) {
            return;
        }
        items_.push_back(std::move(item));
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
    std::set<std::string> seenImports_;
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

// `only` maps the names to list to the labels they are visible under; null lists them all.
void declItems(const Analysis& a, const Module& m, bool qualifiedOnly, const std::map<std::string, std::string>* only,
               Collector& out) {
    bool iface = m.file != nullptr && m.file->isInterface();
    for (const Decl* d : m.decls) {
        if (d == nullptr || !d->name.valid() || (qualifiedOnly && !iface && d->visibility != Visibility::Pub)) {
            continue;
        }
        std::string n = name(a, d->name);
        if (only != nullptr) {
            auto it = only->find(n);
            if (it == only->end()) {
                continue;
            }
            n = it->second;
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

void topLevelNames(const Analysis& a, const Module& current, const ImportScope& scope, Collector& out) {
    std::map<std::string, std::map<std::string, std::string>> named; // package -> name -> local label
    for (const auto& [local, target] : scope.names) {
        named[target.first][target.second] = local;
    }
    for (const Module* m : a.modules()) {
        if (m == nullptr) {
            continue;
        }
        std::string pkg = joinPath(a, m->package);
        bool iface = m->file != nullptr && m->file->isInterface();
        bool own = m == &current || pkg == scope.package || (iface && pkg.empty());
        if (own || scope.star.contains(pkg)) {
            declItems(a, *m, !own, nullptr, out);
        } else if (auto it = named.find(pkg); it != named.end()) {
            declItems(a, *m, true, &it->second, out);
        }
    }
}

int exportItemKind(ExportKind k) {
    switch (k) {
    case ExportKind::Function:
        return kFunction;
    case ExportKind::Struct:
        return kStruct;
    case ExportKind::Enum:
        return kEnum;
    case ExportKind::Trait:
        return kInterface;
    case ExportKind::Const:
        return kConstant;
    }
    return kFunction;
}

// Public symbols of other packages: qualified when their package is imported whole, else with an import edit.
void importableNames(const Analysis& a, std::uint32_t file, const ImportScope& scope, Collector& out) {
    for (Export& e : exports(a)) {
        if (scope.imports(e.package, e.name)) {
            continue;
        }
        Completion item{e.name, exportItemKind(e.kind), std::move(e.detail), e.package, {}, {}};
        if (std::string q = scope.qualifierOf(e.package); !q.empty()) {
            item.insertText = q + "." + e.name;
        } else {
            item.edits.push_back(importEdit(a, file, e.package, e.name));
        }
        out.addFrom(std::move(item));
    }
}

// The package a dotted qualifier such as `math` or `org.quartzlang.math` names, or empty.
std::string qualifiedPackage(const Analysis& a, const ImportScope& scope, std::string_view path) {
    std::string_view head = path.substr(0, path.find('.'));
    std::string full(path);
    if (auto it = scope.qualifiers.find(std::string(head)); it != scope.qualifiers.end()) {
        full = it->second + std::string(path.substr(head.size()));
    }
    for (const Module* m : a.modules()) {
        if (m != nullptr && !m->package.empty() && joinPath(a, m->package) == full) {
            return full;
        }
    }
    return {};
}

// A struct or enum named `n` declared in package `pkg`, or 0.
std::uint32_t typeIn(const Analysis& a, std::string_view pkg, std::string_view n) {
    const sema::Program* p = a.program();
    if (p == nullptr) {
        return 0;
    }
    auto matches = [&](const Decl* d) {
        const Module* m = d != nullptr ? a.module(d->span.file) : nullptr;
        return m != nullptr && d->name.valid() && a.names().name(d->name) == n && joinPath(a, m->package) == pkg;
    };
    for (const auto& [t, d] : p->enumDecls) {
        if (matches(d)) {
            return t;
        }
    }
    for (const auto& [t, d] : p->structDecls) {
        if (matches(d)) {
            return t;
        }
    }
    return 0;
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

// After `pkg.` or `pkg.Type.`: the package's public symbols, or the type's variants and static methods.
bool qualifiedItems(const Analysis& a, const ImportScope& scope, std::string_view path, Collector& out) {
    if (path.empty()) {
        return false;
    }
    if (std::string pkg = qualifiedPackage(a, scope, path); !pkg.empty()) {
        for (Export& e : exports(a)) {
            if (e.package == pkg) {
                out.add(e.name, exportItemKind(e.kind), std::move(e.detail));
            }
        }
        return true;
    }
    std::size_t dot = path.rfind('.');
    std::string pkg = dot == std::string_view::npos ? std::string{} : qualifiedPackage(a, scope, path.substr(0, dot));
    if (pkg.empty()) {
        return false;
    }
    if (std::uint32_t t = typeIn(a, pkg, path.substr(dot + 1))) {
        memberItems(a, t, true, out);
        return true;
    }
    return false;
}

// In an `import` or `package` line: the next segment of the known package names, then the package's symbols.
void importPathItems(const Analysis& a, std::string_view typed, Collector& out) {
    std::size_t dot = typed.rfind('.');
    std::string prefix(dot == std::string_view::npos ? std::string_view{} : typed.substr(0, dot + 1));
    for (const Module* m : a.modules()) {
        std::string pkg = m != nullptr ? joinPath(a, m->package) : std::string{};
        if (!pkg.empty() && pkg.starts_with(prefix) && pkg.size() > prefix.size()) {
            std::string rest = pkg.substr(prefix.size());
            out.add(rest.substr(0, rest.find('.')), kModule);
        }
    }
    if (prefix.empty()) {
        return;
    }
    prefix.pop_back();
    for (Export& e : exports(a)) {
        if (e.package == prefix) {
            out.add(e.name, exportItemKind(e.kind), std::move(e.detail));
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
    std::uint32_t pathStart = start;
    while (pathStart > 0 && (isIdentChar(text[pathStart - 1]) || text[pathStart - 1] == '.')) {
        --pathStart;
    }
    std::uint32_t line = pathStart;
    while (line > 0 && text[line - 1] != '\n') {
        --line;
    }
    std::string_view before = text.substr(line, pathStart - line);
    std::size_t lead = before.find_first_not_of(" \t");
    before = lead == std::string_view::npos ? std::string_view{} : before.substr(lead);
    if (before == "import " || before == "package ") {
        importPathItems(a, text.substr(pathStart, start - pathStart), out);
        return out.take();
    }
    ImportScope scope = importScope(a, *m);
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
        bool value = recv != nullptr && recv->kind == ExprKind::Ident &&
                     static_cast<const IdentExpr*>(recv)->ref != IdentRef::Unresolved &&
                     static_cast<const IdentExpr*>(recv)->ref != IdentRef::Type;
        if (!safe && !value && qualifiedItems(a, scope, text.substr(pathStart, dot - pathStart), out)) {
            return out.take();
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
    topLevelNames(a, *m, scope, out);
    for (const BuiltinFunction& f : builtinFunctions()) {
        if (f.package.empty()) {
            out.add(std::string(f.name), kFunction, std::string(f.signature));
        }
        for (const auto& [local, target] : scope.names) {
            if (target.first == f.package && target.second == f.name) {
                out.add(local, kFunction, std::string(f.signature));
            }
        }
        if (scope.star.contains(std::string(f.package))) {
            out.add(std::string(f.name), kFunction, std::string(f.signature));
        }
    }
    for (std::string_view t : builtinTypes()) {
        out.add(std::string(t), kStruct);
    }
    for (std::string_view k : keywords()) {
        out.add(std::string(k), kKeyword);
    }
    importableNames(a, file, scope, out);
    return out.take();
}

} // namespace quartz::lsp
