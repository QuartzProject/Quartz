#include "Index.hpp"

#include "Imports.hpp"
#include "Query.hpp"
#include "Walk.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace quartz::lsp {

using namespace syntax;

namespace {

bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

class Indexer : public Walker {
public:
    Indexer(const Analysis& a, std::vector<Occurrence>& out) : Walker(a), out_(out) {}

    void run(std::uint32_t file) {
        if (const Module* m = a_.module(file)) {
            file_ = file;
            text_ = a_.input(file).text;
            module(*m);
        }
    }

protected:
    void onImport(const Import& imp) override {
        if (imp.star || imp.path.empty()) {
            return;
        }
        std::vector<Span> words = identifiers(imp.span);
        std::size_t first = 1 + imp.path.size(); // after `import` and the path
        if (!imp.names.empty()) {
            std::string pkg = joinPath(a_, imp.path);
            for (std::size_t i = first; i < words.size(); ++i) {
                importedName(pkg, words[i]);
            }
        } else if (imp.path.size() >= 2 && words.size() >= first) {
            importedName(joinPath(a_, imp.path.first(imp.path.size() - 1)), words[first - 1]);
        }
    }

    void onDecl(const Decl& d) override {
        if (d.kind == DeclKind::Impl || d.kind == DeclKind::Trait) {
            container_ = &d;
        }
        if (!d.name.valid() || d.kind == DeclKind::Impl) {
            return;
        }
        EntityKind k = declKind(d);
        emit(d.nameSpan, &d, k, d.nameSpan.file, true, d.name);
    }

    void onParam(const Param& p) override {
        if (p.local != nullptr) {
            params_.insert(p.local);
            emit(p.span, p.local, EntityKind::Param, p.span.file, true, p.name);
        }
    }

    void onField(const FieldDecl& f, const StructDecl&) override {
        emit(f.span, &f, EntityKind::Field, f.span.file, true, f.name);
    }

    void onVariant(const Variant& v, const EnumDecl&) override {
        emit(v.span, &v, EntityKind::Variant, v.span.file, true, v.name);
    }

    void onStmt(const Stmt& s) override {
        if (s.kind == StmtKind::Let) {
            const auto& l = static_cast<const LetStmt&>(s);
            if (l.local != nullptr) {
                emit(l.nameSpan, l.local, EntityKind::Local, file_, true, l.name, l.mutable_);
            }
        } else if (s.kind == StmtKind::For) {
            const auto& f = static_cast<const ForStmt&>(s);
            Span name = f.nameSpan;
            if (f.indexName.valid()) {
                if (f.indexLocal != nullptr) {
                    emit(f.nameSpan, f.indexLocal, EntityKind::Local, file_, true, f.indexName);
                }
                std::uint32_t end = f.iterable != nullptr ? f.iterable->span.begin : f.span.end;
                name = findIdent({file_, f.nameSpan.end, end}, f.name).value_or(Span{});
            }
            if (f.local != nullptr && name.end > name.begin) {
                emit(name, f.local, EntityKind::Local, file_, true, f.name);
            }
        }
    }

    void onExpr(const Expr& e, const Expr* parent) override {
        switch (e.kind) {
        case ExprKind::Ident:
            ident(static_cast<const IdentExpr&>(e), parent);
            break;
        case ExprKind::Call:
            call(static_cast<const CallExpr&>(e));
            break;
        case ExprKind::Member:
            if (parent == nullptr || parent->kind != ExprKind::Call ||
                static_cast<const CallExpr*>(parent)->callee != &e) {
                member(static_cast<const MemberExpr&>(e));
            }
            break;
        case ExprKind::If: {
            const auto& i = static_cast<const IfExpr&>(e);
            if (i.bindLocal != nullptr && i.bindValue != nullptr) {
                if (auto s = findIdent({file_, e.span.begin, i.bindValue->span.begin}, i.bindName)) {
                    emit(*s, i.bindLocal, EntityKind::Local, file_, true, i.bindName);
                }
            }
            break;
        }
        default:
            break;
        }
    }

    void onType(const TypeExpr& t) override {
        if (t.kind != TypeExprKind::Named) {
            return;
        }
        const auto& n = static_cast<const NamedTypeExpr&>(t);
        if (n.path.empty()) {
            return;
        }
        std::vector<Span> words = identifiers(t.span);
        if (words.size() < n.path.size()) {
            return;
        }
        Span name = words[n.path.size() - 1];
        if (const Decl* d = typeDecl(a_, t.type)) {
            emit(name, d, declKind(*d), d->nameSpan.file, false, d->name);
        } else if (t.type == 0) {
            if (const Decl* trait = traitNamed(n.path.back())) {
                emit(name, trait, EntityKind::Trait, trait->nameSpan.file, false, trait->name);
            }
        }
    }

    void onPattern(const Pattern& p, std::uint32_t subject) override {
        if (p.kind == PatternKind::Binding && p.local != nullptr) {
            if (auto s = findIdent(p.span, p.name)) {
                emit(*s, p.local, EntityKind::Local, file_, true, p.name);
            }
            return;
        }
        if (p.kind != PatternKind::Variant || p.path.empty()) {
            return;
        }
        const EnumDecl* e = enumDeclOf(a_, subject);
        std::vector<Span> words = identifiers(p.span);
        if (e == nullptr || p.variantIndex >= e->variants.size() || words.size() < p.path.size()) {
            return;
        }
        const Variant& v = e->variants[p.variantIndex];
        emit(words[p.path.size() - 1], &v, EntityKind::Variant, v.span.file, false, v.name);
        if (p.path.size() >= 2) {
            emit(words[p.path.size() - 2], static_cast<const Decl*>(e), EntityKind::Enum, e->nameSpan.file, false,
                 e->name);
        }
    }

private:
    EntityKind declKind(const Decl& d) const {
        switch (d.kind) {
        case DeclKind::Fn:
            return container_ != nullptr && d.span.begin >= container_->span.begin &&
                           d.span.end <= container_->span.end && d.span.file == container_->span.file
                       ? EntityKind::Method
                       : EntityKind::Function;
        case DeclKind::Enum:
            return EntityKind::Enum;
        case DeclKind::Trait:
            return EntityKind::Trait;
        case DeclKind::Const:
            return EntityKind::Const;
        default:
            return EntityKind::Struct;
        }
    }

    void ident(const IdentExpr& id, const Expr* parent) {
        switch (id.ref) {
        case IdentRef::Local:
            if (id.local != nullptr) {
                EntityKind k = params_.contains(id.local) ? EntityKind::Param : EntityKind::Local;
                emit(id.span, id.local, k, file_, false, id.local->name, id.local->mutable_);
            }
            break;
        case IdentRef::Function:
            function(id.span, id.fn);
            break;
        case IdentRef::Global:
        case IdentRef::Const:
            if (id.global != nullptr && id.global->decl != nullptr) {
                const ConstDecl* c = id.global->decl;
                emit(id.span, static_cast<const Decl*>(c), EntityKind::Const, c->nameSpan.file, false, c->name);
            }
            break;
        case IdentRef::Type:
            if (parent != nullptr && parent->kind == ExprKind::Call &&
                static_cast<const CallExpr*>(parent)->callee == &id) {
                typeAt(id.span, parent->type);
            } else if (parent == nullptr || parent->kind != ExprKind::Member) {
                typeAt(id.span, typeNamed(a_, a_.names().name(id.name)));
            }
            break;
        default:
            break;
        }
    }

    void call(const CallExpr& c) {
        if (c.callee != nullptr && c.callee->kind == ExprKind::Member) {
            const auto* m = static_cast<const MemberExpr*>(c.callee);
            Span name = memberName(*m);
            if (c.callKind == CallKind::Construct) {
                typeAt(name, c.type);
            } else if (c.callKind == CallKind::Variant) {
                variantAt(name, c.type, c.variant);
                enumObject(*m, c.type);
            } else if (c.fn != nullptr) {
                function(name, c.fn);
            }
        }
        const Decl* target = c.callKind == CallKind::Construct ? typeDecl(a_, c.type)
                             : c.fn != nullptr                 ? c.fn->decl
                                                               : nullptr;
        if (target == nullptr) {
            return;
        }
        for (const Arg& arg : c.args) {
            if (arg.name.valid() && arg.value != nullptr) {
                namedArg(arg, *target);
            }
        }
    }

    void member(const MemberExpr& m) {
        Span name = memberName(m);
        if (m.isVariant) {
            variantAt(name, m.type, m.fieldIndex);
            enumObject(m, m.type);
            return;
        }
        if (m.global != nullptr && m.global->decl != nullptr) {
            const ConstDecl* c = m.global->decl;
            emit(name, static_cast<const Decl*>(c), EntityKind::Const, c->nameSpan.file, false, c->name);
            return;
        }
        if (m.isProperty || m.object == nullptr) {
            return;
        }
        if (const StructDecl* s = structOf(m.object->type)) {
            if (m.fieldIndex < s->fields.size() && s->fields[m.fieldIndex].name == m.name) {
                const FieldDecl& f = s->fields[m.fieldIndex];
                emit(name, &f, EntityKind::Field, f.span.file, false, f.name);
            }
        }
    }

    // The enum named before `.Variant`: `Shape` or the last part of `geo.Shape`.
    void enumObject(const MemberExpr& m, std::uint32_t type) {
        if (m.object == nullptr) {
            return;
        }
        if (m.object->kind == ExprKind::Ident && static_cast<const IdentExpr*>(m.object)->ref == IdentRef::Type) {
            typeAt(m.object->span, type);
        } else if (m.object->kind == ExprKind::Member) {
            typeAt(memberName(*static_cast<const MemberExpr*>(m.object)), type);
        }
    }

    void namedArg(const Arg& arg, const Decl& target) {
        std::uint32_t at = arg.value->span.begin;
        while (at > 0 && (text_[at - 1] == ' ' || text_[at - 1] == '\t' || text_[at - 1] == '\n')) {
            --at;
        }
        if (at == 0 || text_[at - 1] != '=') {
            return;
        }
        --at;
        while (at > 0 && (text_[at - 1] == ' ' || text_[at - 1] == '\t')) {
            --at;
        }
        std::uint32_t end = at;
        while (at > 0 && isIdentChar(text_[at - 1])) {
            --at;
        }
        Span name{file_, at, end};
        if (target.kind == DeclKind::Fn) {
            for (const Param& p : static_cast<const FnDecl&>(target).params) {
                if (p.name == arg.name && p.local != nullptr) {
                    emit(name, p.local, EntityKind::Param, p.span.file, false, p.name);
                }
            }
        } else if (target.kind == DeclKind::Struct || target.kind == DeclKind::State) {
            for (const FieldDecl& f : static_cast<const StructDecl&>(target).fields) {
                if (f.name == arg.name) {
                    emit(name, &f, EntityKind::Field, f.span.file, false, f.name);
                }
            }
        }
    }

    void importedName(const std::string& pkg, Span word) {
        std::string_view n = slice(word);
        for (const Module* m : a_.modules()) {
            if (m == nullptr || joinPath(a_, m->package) != pkg) {
                continue;
            }
            for (const Decl* d : m->decls) {
                if (d != nullptr && d->kind != DeclKind::Impl && d->name.valid() && a_.names().name(d->name) == n) {
                    emit(word, d, declKind(*d), d->nameSpan.file, false, d->name);
                }
            }
        }
    }

    void function(Span span, const sema::FnInfo* fn) {
        if (fn == nullptr || fn->decl == nullptr) {
            return;
        }
        const Decl* d = fn->decl;
        EntityKind k = fn->hasSelf || fn->owner != ir::kError ? EntityKind::Method : EntityKind::Function;
        emit(span, d, k, d->nameSpan.file, false, d->name);
    }

    void typeAt(Span span, std::uint32_t type) {
        if (const Decl* d = typeDecl(a_, type)) {
            emit(span, d, declKind(*d), d->nameSpan.file, false, d->name);
        }
    }

    void variantAt(Span span, std::uint32_t type, std::uint32_t index) {
        const EnumDecl* e = enumDeclOf(a_, type);
        if (e != nullptr && index < e->variants.size()) {
            const Variant& v = e->variants[index];
            emit(span, &v, EntityKind::Variant, v.span.file, false, v.name);
        }
    }

    const StructDecl* structOf(std::uint32_t type) const {
        const sema::Program* p = a_.program();
        if (p != nullptr && type != 0 && type < p->types.size() && p->types.kind(type) == ir::TypeKind::Nullable) {
            type = p->types.info(type).elem;
        }
        const Decl* d = typeDecl(a_, type);
        return d != nullptr && d->kind != DeclKind::Enum ? static_cast<const StructDecl*>(d) : nullptr;
    }

    const Decl* traitNamed(Symbol name) const {
        for (const Module* m : a_.modules()) {
            for (const Decl* d : m != nullptr ? m->decls : std::span<Decl*>{}) {
                if (d != nullptr && d->kind == DeclKind::Trait && d->name == name) {
                    return d;
                }
            }
        }
        return nullptr;
    }

    Span memberName(const MemberExpr& m) const {
        Span s = m.span;
        std::size_t len = m.name.valid() ? a_.names().name(m.name).size() : 0;
        if (s.end >= len && s.end - len >= s.begin) {
            s.begin = static_cast<std::uint32_t>(s.end - len);
        }
        return s;
    }

    std::string_view slice(Span s) const {
        if (s.file != file_ || s.begin > s.end || s.end > text_.size()) {
            return {};
        }
        return text_.substr(s.begin, s.end - s.begin);
    }

    // The identifier-like words of a span of the current file, in order.
    std::vector<Span> identifiers(Span range) const {
        std::vector<Span> out;
        if (range.file != file_ || range.end > text_.size()) {
            return out;
        }
        std::uint32_t at = range.begin;
        while (at < range.end) {
            if (!isIdentChar(text_[at])) {
                ++at;
                continue;
            }
            std::uint32_t begin = at;
            while (at < range.end && isIdentChar(text_[at])) {
                ++at;
            }
            out.push_back({file_, begin, at});
        }
        return out;
    }

    std::optional<Span> findIdent(Span range, Symbol name) const {
        if (!name.valid()) {
            return std::nullopt;
        }
        std::string_view n = a_.names().name(name);
        for (Span s : identifiers(range)) {
            if (slice(s) == n) {
                return s;
            }
        }
        return std::nullopt;
    }

    void emit(Span span, const void* key, EntityKind kind, std::uint32_t declFile, bool declaration, Symbol name,
              bool mutable_ = false) {
        if (key == nullptr || span.file != file_ || span.end <= span.begin || span.end > text_.size() ||
            declFile >= a_.fileCount()) {
            return;
        }
        bool exact = name.valid() && slice(span) == a_.names().name(name);
        out_.push_back({span, key, kind, declFile, declaration, exact, mutable_});
    }

    std::vector<Occurrence>& out_;
    std::uint32_t file_ = 0;
    std::string_view text_;
    const Decl* container_ = nullptr;
    std::unordered_set<const sema::Local*> params_;
};

} // namespace

Index::Index(const Analysis& a) {
    Indexer indexer(a, all_);
    fileStart_.assign(a.fileCount() + 1, 0);
    for (std::uint32_t f = 0; f < a.fileCount(); ++f) {
        fileStart_[f] = static_cast<std::uint32_t>(all_.size());
        if (!a.input(f).uri.empty()) {
            std::size_t before = all_.size();
            indexer.run(f);
            std::stable_sort(all_.begin() + static_cast<std::ptrdiff_t>(before), all_.end(),
                             [](const Occurrence& x, const Occurrence& y) { return x.span.begin < y.span.begin; });
        }
    }
    fileStart_[a.fileCount()] = static_cast<std::uint32_t>(all_.size());
    for (std::uint32_t i = 0; i < all_.size(); ++i) {
        byKey_[all_[i].key].push_back(i);
    }
}

std::span<const Occurrence> Index::inFile(std::uint32_t file) const {
    if (file + 1 >= fileStart_.size()) {
        return {};
    }
    return std::span<const Occurrence>(all_).subspan(fileStart_[file], fileStart_[file + 1] - fileStart_[file]);
}

const Occurrence* Index::at(std::uint32_t file, std::uint32_t offset) const {
    const Occurrence* touching = nullptr;
    for (const Occurrence& o : inFile(file)) {
        if (o.span.begin <= offset && offset < o.span.end) {
            return &o;
        }
        if (o.span.end == offset && touching == nullptr) {
            touching = &o;
        }
    }
    return touching;
}

std::vector<const Occurrence*> Index::of(const void* key) const {
    std::vector<const Occurrence*> out;
    if (auto it = byKey_.find(key); it != byKey_.end()) {
        for (std::uint32_t i : it->second) {
            out.push_back(&all_[i]);
        }
    }
    return out;
}

bool Index::declaredInUserFile(const void* key) const {
    auto it = byKey_.find(key);
    if (it == byKey_.end()) {
        return false;
    }
    return std::ranges::any_of(it->second, [&](std::uint32_t i) { return all_[i].declaration; });
}

} // namespace quartz::lsp
