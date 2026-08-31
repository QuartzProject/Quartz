#include "sema/SemaImpl.hpp"

#include <algorithm>
#include <format>

namespace quartz::sema {

namespace {

std::string joinPath(const Interner& names, std::span<const Symbol> path) {
    std::string out;
    for (std::size_t i = 0; i < path.size(); ++i) {
        out += (i ? "." : "") + std::string(names.name(path[i]));
    }
    return out;
}

bool silenced(std::string_view name) { return name.empty() || name.starts_with('_') || name == "self"; }

// The value a literal pattern stands for, comparable across patterns: kind tag, then the bits or the text.
std::optional<std::pair<int, std::string>> literalKey(const Expr* e) {
    if (e == nullptr) {
        return std::nullopt;
    }
    switch (e->kind) {
    case ExprKind::IntLit:
        return std::pair(1, std::to_string(static_cast<const IntLitExpr*>(e)->value));
    case ExprKind::CharLit:
        return std::pair(2, std::to_string(static_cast<const CharLitExpr*>(e)->value));
    case ExprKind::StringLit:
        return std::pair(3, std::string(static_cast<const StringLitExpr*>(e)->value));
    case ExprKind::BoolLit:
        return std::pair(4, static_cast<const BoolLitExpr*>(e)->value ? "1" : "0");
    case ExprKind::NullLit:
        return std::pair(5, std::string());
    case ExprKind::Unary: {
        const auto* u = static_cast<const UnaryExpr*>(e);
        if (u->op == UnaryOp::Neg && u->operand->kind == ExprKind::IntLit) {
            return std::pair(6, std::to_string(static_cast<const IntLitExpr*>(u->operand)->value));
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

} // namespace

bool Checker::lintable(std::uint32_t file) const {
    if (file >= files_.size() || files_[file].module == nullptr) {
        return false;
    }
    return !files_[file].interface && !files_[file].module->library;
}

void Checker::collectFixedSignatures() {
    for (Module* mod : modules_) {
        for (Decl* d : mod->decls) {
            if (d->kind == DeclKind::Impl && static_cast<ImplDecl*>(d)->trait != nullptr) {
                for (FnDecl* m : static_cast<ImplDecl*>(d)->methods) {
                    fixedSignatures_.insert(m);
                }
            } else if (d->kind == DeclKind::Trait) {
                for (FnDecl* m : static_cast<TraitDecl*>(d)->methods) {
                    fixedSignatures_.insert(m);
                }
            }
        }
    }
}

void Checker::noteShadowing(Symbol name, Span span) {
    if (!lintFn_ || shadowOk_ || silenced(str(name))) {
        return;
    }
    for (const auto& scope : scopes_) {
        if (scope.contains(name)) {
            diags_.warning(span, rule::kShadowedVariable,
                           std::format("'{}' shadows another local variable of this function", str(name)));
            return;
        }
    }
}

void Checker::noteImportUse(Symbol alias, std::uint32_t declFile) const {
    if (currentFile_ < files_.size() && files_[currentFile_].symbolImports.contains(alias)) {
        usedSymbolImports_.insert({currentFile_, alias.id});
    }
    if (declFile < files_.size()) {
        notePackageUse(files_[declFile].package);
    }
}

void Checker::notePackageUse(const std::string& package) const {
    if (currentFile_ < files_.size() && files_[currentFile_].package != package) {
        usedPackages_.insert({currentFile_, package});
    }
}

void Checker::noteFnUse(FnInfo* fn) {
    if (fn != checkingFn_) {
        ++fn->uses;
    }
    if (fn->decl != nullptr) {
        notePackageUse(files_[fn->decl->span.file].package);
    }
}

void Checker::warnDiscarded(const Expr* e) {
    if (lintable(currentFile_) && e->type != ir::kError && types_->isResult(e->type)) {
        diags_.warning(e->span, rule::kDiscardedResult,
                       "this 'Result' is discarded; handle the error or write 'let _ = ...'");
    }
}

bool Checker::sameLiteral(const Pattern* a, const Pattern* b) {
    if (a->kind != b->kind || (a->kind != PatternKind::Literal && a->kind != PatternKind::Range)) {
        return false;
    }
    auto lo = literalKey(a->literal);
    if (!lo || lo != literalKey(b->literal)) {
        return false;
    }
    if (a->kind == PatternKind::Literal) {
        return true;
    }
    auto hi = literalKey(a->rangeHi);
    return hi && hi == literalKey(b->rangeHi) && a->inclusive == b->inclusive;
}

// Usefulness (Maranget): true when some value `q` matches is matched by no row; literals only cover themselves.
bool Checker::useful(const std::vector<PatternRow>& rows, const PatternRow& q, const std::vector<TypeId>& cols) const {
    if (cols.empty()) {
        return rows.empty();
    }
    TypeId t = cols[0];
    std::vector<TypeId> rest(cols.begin() + 1, cols.end());
    std::vector<const Pattern*> args;
    int head = q[0] == nullptr ? -1 : headCtor(q[0], t, args);
    std::optional<std::vector<Ctor>> ctors = ctorsOf(t);
    auto specialize = [&](int k, const PatternRow& qrow, std::vector<PatternRow>& outRows, PatternRow& outQ) {
        std::size_t arity = (*ctors)[k].args.size();
        std::vector<const Pattern*> rowArgs;
        for (const PatternRow& r : rows) {
            int h = r[0] == nullptr ? -1 : headCtor(r[0], t, rowArgs);
            if (h == -1) {
                PatternRow row(arity, nullptr);
                row.insert(row.end(), r.begin() + 1, r.end());
                outRows.push_back(std::move(row));
            } else if (h == k) {
                PatternRow row(rowArgs.begin(), rowArgs.end());
                row.insert(row.end(), r.begin() + 1, r.end());
                outRows.push_back(std::move(row));
            }
        }
        int qh = qrow[0] == nullptr ? -1 : headCtor(qrow[0], t, rowArgs);
        outQ = qh == -1 ? PatternRow(arity, nullptr) : PatternRow(rowArgs.begin(), rowArgs.end());
        outQ.insert(outQ.end(), qrow.begin() + 1, qrow.end());
    };
    auto subCols = [&](int k) {
        std::vector<TypeId> sub = (*ctors)[k].args;
        sub.insert(sub.end(), rest.begin(), rest.end());
        return sub;
    };
    if (head >= 0 && ctors && static_cast<std::size_t>(head) < ctors->size()) {
        std::vector<PatternRow> spec;
        PatternRow sq;
        specialize(head, q, spec, sq);
        return useful(spec, sq, subCols(head));
    }
    PatternRow tail(q.begin() + 1, q.end());
    if (head == -1) {
        std::vector<bool> seen(ctors ? ctors->size() : 0, false);
        for (const PatternRow& r : rows) {
            int k = r[0] == nullptr ? -1 : headCtor(r[0], t, args);
            if (k >= 0 && static_cast<std::size_t>(k) < seen.size()) {
                seen[k] = true;
            }
        }
        if (ctors && !seen.empty() && std::all_of(seen.begin(), seen.end(), [](bool b) { return b; })) {
            for (std::size_t k = 0; k < ctors->size(); ++k) {
                std::vector<PatternRow> spec;
                PatternRow sq;
                specialize(static_cast<int>(k), q, spec, sq);
                if (useful(spec, sq, subCols(static_cast<int>(k)))) {
                    return true;
                }
            }
            return false;
        }
    }
    std::vector<PatternRow> covering;
    for (const PatternRow& r : rows) {
        int k = r[0] == nullptr ? -1 : headCtor(r[0], t, args);
        if (k == -1 || (head == -2 && k == -2 && sameLiteral(r[0], q[0]))) {
            covering.emplace_back(r.begin() + 1, r.end());
        }
    }
    return useful(covering, tail, rest);
}

void Checker::warnUnreachableArms(MatchExpr* m, TypeId subject) {
    if (!lintable(currentFile_) || subject == ir::kError) {
        return;
    }
    std::vector<PatternRow> rows;
    for (MatchArm& arm : m->arms) {
        if (!useful(rows, {arm.pattern}, {subject})) {
            diags_.warning(arm.pattern->span, rule::kUnreachablePattern,
                           "this match arm is unreachable: the arms above match every value it matches");
        }
        if (arm.guard == nullptr) {
            rows.push_back({arm.pattern});
        }
    }
}

void Checker::lintImports(const Module& mod) {
    std::uint32_t file = mod.file->id();
    for (const Import& imp : mod.imports) {
        std::string path = joinPath(names_, imp.path);
        if (imp.star) {
            if (!usedPackages_.contains({file, path})) {
                diags_.warning(imp.span, rule::kUnusedImport, std::format("unused import '{}.*'", path));
            }
            continue;
        }
        if (!imp.names.empty()) {
            for (Symbol n : imp.names) {
                if (!usedSymbolImports_.contains({file, n.id})) {
                    diags_.warning(imp.span, rule::kUnusedImport,
                                   std::format("unused import '{}' from '{}'", str(n), path));
                }
            }
            continue;
        }
        bool package = packages_.contains(path);
        Symbol local = imp.alias.valid() ? imp.alias : imp.path[imp.path.size() - 1];
        bool used = package ? usedPackages_.contains({file, path}) : usedSymbolImports_.contains({file, local.id});
        if (!used) {
            diags_.warning(imp.span, rule::kUnusedImport, std::format("unused import '{}'", path));
        }
    }
}

void Checker::lint() {
    if (diags_.hasErrors()) {
        return; // names an error left unresolved would look unused
    }
    for (Module* mod : modules_) {
        if (lintable(mod->file->id())) {
            lintImports(*mod);
        }
    }
    for (const Local* l : lintLocals_) {
        std::string_view name = str(l->name);
        if (silenced(name) || l->reads > 0) {
            continue;
        }
        if (l->param) {
            diags_.warning(l->span, rule::kUnusedParameter, std::format("unused parameter '{}'", name));
        } else if (l->writes > 0) {
            diags_.warning(l->span, rule::kUnusedAssignment,
                           std::format("variable '{}' is assigned but never read", name));
        } else {
            diags_.warning(l->span, rule::kUnusedVariable, std::format("unused variable '{}'", name));
        }
    }
    static constexpr std::string_view kImplicit[] = {"eq", "cmp", "show", "next", "hash"};
    for (const auto& owned : prog_->fnStorage) {
        const FnInfo* fn = owned.get();
        const FnDecl* decl = fn->decl;
        if (decl == nullptr || decl->body == nullptr || fn->uses > 0 || !lintable(decl->span.file) ||
            fixedSignatures_.contains(decl) || !fn->annotations.empty() || silenced(str(decl->name))) {
            continue;
        }
        bool method = fn->owner != ir::kError;
        if (method ? decl->visibility != Visibility::Private ||
                         std::find(std::begin(kImplicit), std::end(kImplicit), str(decl->name)) != std::end(kImplicit)
                   : !decl->privateKeyword) {
            continue;
        }
        diags_.warning(decl->nameSpan, rule::kUnusedPrivate,
                       std::format("unused private {} '{}'", method ? "method" : "function", fn->name));
    }
}

} // namespace quartz::sema
