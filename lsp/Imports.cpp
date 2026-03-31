#include "Imports.hpp"

#include "Builtins.hpp"
#include "Query.hpp"

namespace quartz::lsp {

using namespace syntax;

namespace {

std::set<std::string> packages(const Analysis& a) {
    std::set<std::string> out;
    for (const Module* m : a.modules()) {
        if (m != nullptr && !m->package.empty()) {
            out.insert(joinPath(a, m->package));
        }
    }
    return out;
}

std::uint32_t lineStart(std::string_view text, std::uint32_t offset) {
    while (offset > 0 && text[offset - 1] != '\n') {
        --offset;
    }
    return offset;
}

// End of the `package` line, before its newline; npos when the file declares none.
std::size_t packageLineEnd(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        std::size_t end = nl == std::string_view::npos ? text.size() : nl;
        std::string_view line = text.substr(at, end - at);
        std::size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string_view::npos && !line.substr(first).starts_with("//")) {
            return line.substr(first).starts_with("package ") ? end : std::string_view::npos;
        }
        at = end + 1;
    }
    return std::string_view::npos;
}

} // namespace

std::string joinPath(const Analysis& a, std::span<const Symbol> path) {
    std::string out;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i > 0) {
            out += '.';
        }
        out += path[i].valid() ? a.names().name(path[i]) : std::string_view("?");
    }
    return out;
}

std::vector<Export> exports(const Analysis& a) {
    std::vector<Export> out;
    for (const Module* m : a.modules()) {
        if (m == nullptr || m->package.empty()) {
            continue;
        }
        std::string pkg = joinPath(a, m->package);
        bool iface = m->file != nullptr && m->file->isInterface();
        for (const Decl* d : m->decls) {
            if (d == nullptr || !d->name.valid() || (!iface && d->visibility != Visibility::Pub)) {
                continue;
            }
            Export e{pkg, std::string(a.names().name(d->name)), ExportKind::Function, {}};
            switch (d->kind) {
            case DeclKind::Fn: {
                const auto* fn = static_cast<const FnDecl*>(d);
                e.detail = fn->info != nullptr ? signature(a, *fn->info) : std::string{};
                break;
            }
            case DeclKind::Struct:
            case DeclKind::State:
                e.kind = ExportKind::Struct;
                e.detail = d->kind == DeclKind::State ? "state" : "struct";
                break;
            case DeclKind::Enum:
                e.kind = ExportKind::Enum;
                e.detail = "enum";
                break;
            case DeclKind::Trait:
                e.kind = ExportKind::Trait;
                e.detail = "trait";
                break;
            case DeclKind::Const: {
                const auto* c = static_cast<const ConstDecl*>(d);
                e.kind = ExportKind::Const;
                e.detail = c->info != nullptr ? a.typeName(c->info->type) : std::string{};
                break;
            }
            case DeclKind::Impl:
                continue;
            }
            out.push_back(std::move(e));
        }
    }
    for (const BuiltinFunction& f : builtinFunctions()) {
        if (!f.package.empty()) {
            out.push_back(
                {std::string(f.package), std::string(f.name), ExportKind::Function, "fn " + std::string(f.signature)});
        }
    }
    return out;
}

bool ImportScope::imports(std::string_view pkg, std::string_view name) const {
    if (pkg == package || star.contains(std::string(pkg))) {
        return true;
    }
    for (const auto& [local, target] : names) {
        if (target.first == pkg && target.second == name) {
            return true;
        }
    }
    return false;
}

std::string ImportScope::qualifierOf(std::string_view pkg) const {
    for (const auto& [local, target] : qualifiers) {
        if (target == pkg) {
            return local;
        }
    }
    return {};
}

ImportScope importScope(const Analysis& a, const Module& m) {
    ImportScope scope;
    scope.package = joinPath(a, m.package);
    std::set<std::string> known;
    bool knownLoaded = false;
    for (const Import& imp : m.imports) {
        if (imp.path.empty()) {
            continue;
        }
        std::string path = joinPath(a, imp.path);
        if (imp.star) {
            scope.star.insert(path);
            continue;
        }
        for (Symbol s : imp.names) {
            if (s.valid()) {
                std::string n(a.names().name(s));
                scope.names[n] = {path, n};
            }
        }
        if (!imp.names.empty()) {
            continue;
        }
        if (!knownLoaded) {
            known = packages(a);
            knownLoaded = true;
        }
        Symbol last = imp.path.back();
        std::string local(a.names().name(imp.alias.valid() ? imp.alias : last));
        if (known.contains(path)) {
            scope.qualifiers[local] = path;
        } else if (imp.path.size() >= 2) {
            scope.names[local] = {joinPath(a, imp.path.first(imp.path.size() - 1)), std::string(a.names().name(last))};
        }
    }
    return scope;
}

TextEdit importEdit(const Analysis& a, std::uint32_t file, std::string_view package, std::string_view name) {
    const Module* m = a.module(file);
    std::string_view text = a.input(file).text;
    std::string target = std::string(package) + "." + std::string(name);
    std::string line = "import " + target;
    if (m == nullptr) {
        return {0, 0, line + "\n\n"};
    }
    for (const Import& imp : m->imports) {
        std::uint32_t end = imp.span.end;
        if (!imp.names.empty() && joinPath(a, imp.path) == package && end > 0 && end <= text.size() &&
            text[end - 1] == '}') {
            return {end - 1, end - 1, ", " + std::string(name)};
        }
    }
    const Import* last = nullptr;
    for (const Import& imp : m->imports) {
        if (imp.span.end > text.size()) {
            continue;
        }
        if (target < joinPath(a, imp.path)) {
            std::uint32_t at = lineStart(text, imp.span.begin);
            return {at, at, line + "\n"};
        }
        last = &imp;
    }
    if (last != nullptr) {
        return {last->span.end, last->span.end, "\n" + line};
    }
    std::size_t pkgEnd = packageLineEnd(text);
    if (pkgEnd != std::string_view::npos) {
        auto at = static_cast<std::uint32_t>(pkgEnd);
        return {at, at, "\n\n" + line};
    }
    return {0, 0, line + "\n\n"};
}

} // namespace quartz::lsp
