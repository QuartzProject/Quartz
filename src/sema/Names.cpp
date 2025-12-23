#include "sema/SemaImpl.hpp"

#include <format>

namespace quartz::sema {

namespace {

std::string joinPath(Interner& names, std::span<const Symbol> path) {
    std::string out;
    for (std::size_t i = 0; i < path.size(); ++i) {
        out += (i ? "." : "") + std::string(names.name(path[i]));
    }
    return out;
}

} // namespace

void Checker::indexFiles() {
    files_.resize(modules_.size() + 1);
    for (Module* mod : modules_) {
        std::uint32_t id = mod->file->id();
        if (id >= files_.size()) {
            files_.resize(id + 1);
        }
        FileScope& fs = files_[id];
        fs.package = joinPath(names_, mod->package);
        fs.interface = mod->file->isInterface();
        fs.module = mod;
        packages_.insert(fs.package);
    }
    for (Module* mod : modules_) {
        FileScope& fs = files_[mod->file->id()];
        for (const Import& imp : mod->imports) {
            std::string path = joinPath(names_, imp.path);
            if (imp.star || !imp.names.empty()) {
                if (!packages_.contains(path)) {
                    diags_.error(imp.span, std::format("unknown package '{}'", path));
                    continue;
                }
                if (imp.star) {
                    fs.starImports.push_back(path);
                }
                for (Symbol n : imp.names) {
                    fs.symbolImports[n] = {path, n};
                }
                continue;
            }
            if (imp.path.size() < 2) {
                diags_.error(imp.span, "an import names a package and a symbol, or uses '.*' or '.{...}'");
                continue;
            }
            std::string pkg = joinPath(names_, imp.path.subspan(0, imp.path.size() - 1));
            if (!packages_.contains(pkg)) {
                if (packages_.contains(path)) {
                    fs.starImports.push_back(path); // `import a.b` of a whole package
                } else {
                    diags_.error(imp.span, std::format("unknown package '{}'", pkg));
                }
                continue;
            }
            Symbol last = imp.path[imp.path.size() - 1];
            fs.symbolImports[imp.alias.valid() ? imp.alias : last] = {pkg, last};
        }
    }
}

void Checker::addName(Symbol name, NameKind kind, void* target, const Decl* decl, std::uint32_t file) {
    const FileScope& fs = files_[file];
    Visibility vis = fs.interface ? Visibility::Pub : decl != nullptr ? decl->visibility : Visibility::Pub;
    Span span = decl != nullptr ? decl->span : Span{};
    for (const NameEntry& e : table_[name]) {
        if (files_[e.file].package != fs.package) {
            continue;
        }
        bool clash = e.file == file || (e.vis != Visibility::Private && vis != Visibility::Private);
        bool typeVsValue = (e.kind == NameKind::Type || e.kind == NameKind::Trait) !=
                           (kind == NameKind::Type || kind == NameKind::Trait);
        if (clash && !typeVsValue) {
            diags_.error(span, std::format("'{}' is already declared", str(name)));
            return;
        }
    }
    table_[name].push_back({kind, target, file, vis});
}

bool Checker::visibleFrom(const NameEntry& e, Symbol name, std::uint32_t file) const {
    if (e.file == file) {
        return true;
    }
    const FileScope& to = files_[e.file];
    const FileScope& from = files_[file];
    if (to.interface && to.package.empty()) {
        return true;
    }
    if (to.package == from.package) {
        return e.vis != Visibility::Private;
    }
    if (e.vis != Visibility::Pub) {
        return false;
    }
    for (const std::string& p : from.starImports) {
        if (p == to.package) {
            return true;
        }
    }
    auto it = from.symbolImports.find(name);
    return it != from.symbolImports.end() && it->second.first == to.package && it->second.second == name;
}

void* Checker::findName(Symbol name, NameKind kind, Span span) {
    std::uint32_t file = currentFile_;
    Symbol lookup = name;
    if (file < files_.size()) {
        auto alias = files_[file].symbolImports.find(name);
        if (alias != files_[file].symbolImports.end()) {
            lookup = alias->second.second;
        }
    }
    auto it = table_.find(lookup);
    if (it == table_.end()) {
        return nullptr;
    }
    const NameEntry* found = nullptr;
    for (const NameEntry& e : it->second) {
        if (e.kind != kind || !(visibleFrom(e, lookup, file) || (lookup != name && imported(e, name, file)))) {
            continue;
        }
        if (found != nullptr && files_[found->file].package != files_[e.file].package) {
            diags_.error(span, std::format("'{}' is ambiguous: it is visible from '{}' and '{}'", str(name),
                                           files_[found->file].package, files_[e.file].package));
            return found->target;
        }
        if (found == nullptr || e.file == file) {
            found = &e;
        }
    }
    return found != nullptr ? found->target : nullptr;
}

void* Checker::findIn(const std::string& package, Symbol name, NameKind kind) {
    auto it = table_.find(name);
    if (it == table_.end()) {
        return nullptr;
    }
    for (const NameEntry& e : it->second) {
        if (e.kind == kind && files_[e.file].package == package &&
            (e.vis == Visibility::Pub || files_[currentFile_].package == package)) {
            return e.target;
        }
    }
    return nullptr;
}

// `a.b.c` used as an expression names a package when `a` is not a value in scope and the path is a known package.
bool Checker::packagePath(const Expr* e, std::string& out) {
    std::vector<Symbol> parts;
    while (e->kind == ExprKind::Member && !static_cast<const MemberExpr*>(e)->safe) {
        parts.push_back(static_cast<const MemberExpr*>(e)->name);
        e = static_cast<const MemberExpr*>(e)->object;
    }
    if (e->kind != ExprKind::Ident) {
        return false;
    }
    Symbol root = static_cast<const IdentExpr*>(e)->name;
    if (findLocal(root) != nullptr || findGlobal(root) != nullptr || findType(root) != nullptr) {
        return false;
    }
    out = std::string(str(root));
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        out += "." + std::string(str(*it));
    }
    return packages_.contains(out);
}

bool Checker::imported(const NameEntry& e, Symbol alias, std::uint32_t file) const {
    auto it = files_[file].symbolImports.find(alias);
    return it != files_[file].symbolImports.end() && e.vis == Visibility::Pub &&
           it->second.first == files_[e.file].package;
}

} // namespace quartz::sema
