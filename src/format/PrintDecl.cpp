#include "format/Printer.hpp"

#include <algorithm>

namespace quartz::format {

namespace {

bool isModifier(Tok k) { return k == Tok::KwSuspend || k == Tok::KwPub || k == Tok::KwInternal || k == Tok::KwPrivate; }

} // namespace

void Printer::flushAfterToken(std::uint32_t begin) {
    std::size_t i = tokenAt(begin);
    if (i + 1 < toks_.size()) {
        flush(toks_[i + 1].span.begin);
    }
}

std::uint32_t Printer::nextTokenBegin(std::uint32_t end) const {
    std::size_t i = tokenEndingAt(end);
    return i + 1 < toks_.size() ? toks_[i + 1].span.begin : static_cast<std::uint32_t>(src_.size());
}

std::uint32_t Printer::paramEnd(const Param& p) const {
    if (p.defaultValue != nullptr) {
        return outer(p.defaultValue).end;
    }
    return p.type != nullptr ? p.type->span.end : p.span.end;
}

void Printer::path(std::span<const Symbol> parts) {
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            text(".");
        }
        text(name(parts[i]));
    }
}

void Printer::module(const Module& m) {
    bool first = true;
    auto item = [&](std::uint32_t begin, auto&& print) {
        if (!first) {
            newline(begin);
        }
        first = false;
        flush(begin);
        if (blankLineBefore(begin)) {
            blankLine();
        }
        Scope scope(*this);
        print();
    };
    if (!m.package.empty()) {
        item(toks_[nextToken(0, Tok::KwPackage)].span.begin, [&] {
            text("package");
            space();
            path(m.package);
        });
    }
    for (const Import& imp : m.imports) {
        item(imp.span.begin, [&] {
            text("import");
            space();
            path(imp.path);
            if (imp.star) {
                text(".*");
            } else if (!imp.names.empty()) {
                text(".{");
                for (std::size_t i = 0; i < imp.names.size(); ++i) {
                    if (i > 0) {
                        text(",");
                        space();
                    }
                    text(name(imp.names[i]));
                }
                std::size_t close = nextToken(imp.span.begin, Tok::RBrace);
                if (close < toks_.size()) {
                    flush(toks_[close].span.begin);
                }
                text("}");
            }
            if (imp.alias.valid()) {
                space();
                text("as");
                space();
                text(name(imp.alias));
            }
        });
    }
    for (const Decl* d : m.decls) {
        item(declBegin(*d), [&] { decl(*d, false); });
    }
}

std::uint32_t Printer::declBegin(const Decl& d) const {
    std::size_t i = tokenAt(d.span.begin);
    while (i > 0 && i < toks_.size() && isModifier(toks_[i - 1].kind)) {
        --i;
    }
    std::uint32_t begin = i < toks_.size() ? toks_[i].span.begin : d.span.begin;
    return d.attrs.empty() ? begin : std::min(begin, d.attrs[0].span.begin);
}

void Printer::attributes(std::span<const Attribute> attrs) {
    for (const Attribute& a : attrs) {
        flush(a.span.begin);
        text("@");
        flushAfterToken(a.span.begin);
        text(name(a.name));
        if (src_[a.span.end - 1] == ')') {
            std::size_t open = nextToken(a.span.begin, Tok::LParen);
            auto argBegin = [&](std::size_t i) {
                std::uint32_t b = outer(a.args[i].value, open).begin;
                std::size_t t = tokenAt(b);
                return a.args[i].name.valid() && t >= 2 && t < toks_.size() ? toks_[t - 2].span.begin : b;
            };
            items(
                "(", ")", toks_[open].span.end, a.span.end - 1, a.args.size(), argBegin,
                [&](std::size_t i) { return outer(a.args[i].value, open).end; },
                [&](std::size_t i) {
                    if (a.args[i].name.valid()) {
                        flush(argBegin(i));
                        text(name(a.args[i].name));
                        space();
                        text("=");
                        space();
                    }
                    expr(a.args[i].value, kPrecLowest, open);
                });
        }
        newline(nextTokenBegin(a.span.end));
    }
}

void Printer::visibility(const Decl& d, bool inTrait) {
    if (inTrait) {
        return;
    }
    std::size_t i = tokenAt(d.span.begin);
    if (i > 0 && i < toks_.size() && toks_[i - 1].kind == Tok::KwSuspend) {
        --i;
    }
    if (i == 0 || i >= toks_.size()) {
        return;
    }
    switch (toks_[i - 1].kind) {
    case Tok::KwPub:
        text("pub");
        break;
    case Tok::KwInternal:
        text("internal");
        break;
    case Tok::KwPrivate:
        text("private");
        break;
    default:
        return;
    }
    space();
}

void Printer::decl(const Decl& d, bool inTrait) {
    attributes(d.attrs);
    visibility(d, inTrait);
    std::uint32_t closeAt = d.span.end - 1;
    switch (d.kind) {
    case DeclKind::Fn:
        fn(static_cast<const FnDecl&>(d));
        return;
    case DeclKind::Struct:
    case DeclKind::State:
        structDecl(static_cast<const StructDecl&>(d));
        return;
    case DeclKind::Enum:
        enumDecl(static_cast<const EnumDecl&>(d));
        return;
    case DeclKind::Impl: {
        const auto& impl = static_cast<const ImplDecl&>(d);
        text("impl");
        typeParams(impl.typeParams);
        space();
        if (impl.trait != nullptr) {
            type(impl.trait);
            space();
            text("for");
            space();
        }
        type(impl.target);
        space();
        methods(impl.methods, closeAt, false);
        return;
    }
    case DeclKind::Trait: {
        const auto& t = static_cast<const TraitDecl&>(d);
        text("trait");
        space();
        flushAfterToken(d.span.begin);
        text(name(t.name));
        space();
        methods(t.methods, closeAt, true);
        return;
    }
    case DeclKind::Const: {
        const auto& c = static_cast<const ConstDecl&>(d);
        text("const");
        space();
        flushAfterToken(d.span.begin);
        text(name(c.name));
        if (c.type != nullptr) {
            text(":");
            space();
            type(c.type);
        }
        space();
        text("=");
        space();
        expr(c.value);
        return;
    }
    }
}

void Printer::methods(std::span<FnDecl* const> fns, std::uint32_t closeAt, bool inTrait) {
    text("{");
    if (fns.empty() && !commentBefore(closeAt)) {
        text("}");
        return;
    }
    {
        Indent in(*this);
        for (const FnDecl* f : fns) {
            std::uint32_t begin = declBegin(*f);
            newline(begin);
            flush(begin);
            if (blankLineBefore(begin)) {
                blankLine();
            }
            Scope scope(*this);
            decl(*f, inTrait);
        }
        newline(closeAt);
        flush(closeAt);
    }
    newline(closeAt);
    text("}");
}

void Printer::typeParams(std::span<const TypeParam> params) {
    if (params.empty()) {
        return;
    }
    text("<");
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i > 0) {
            text(",");
            space();
        }
        flush(params[i].span.begin);
        text(name(params[i].name));
        for (std::size_t b = 0; b < params[i].bounds.size(); ++b) {
            text(b == 0 ? ":" : " +");
            space();
            text(name(params[i].bounds[b]));
        }
    }
    std::size_t close = nextToken(params.back().span.begin, Tok::Gt);
    if (close < toks_.size()) {
        flush(toks_[close].span.begin);
    }
    text(">");
}

void Printer::param(const Param& p) {
    flush(p.span.begin);
    text(name(p.name));
    if (p.type != nullptr) {
        text(":");
        space();
        type(p.type);
    }
    if (p.defaultValue != nullptr) {
        space();
        text("=");
        space();
        expr(p.defaultValue);
    }
}

void Printer::fn(const FnDecl& f) {
    if (f.isSuspend) {
        text("suspend");
        space();
    }
    text("fn");
    space();
    flushAfterToken(f.span.begin);
    text(name(f.name));
    typeParams(f.typeParams);
    std::size_t open = nextToken(f.span.begin, Tok::LParen);
    std::size_t close = matching(open);
    std::size_t self = f.hasSelf ? 1 : 0;
    auto begin = [&](std::size_t i) { return i < self ? toks_[open + 1].span.begin : f.params[i - self].span.begin; };
    auto end = [&](std::size_t i) { return i < self ? toks_[open + 1].span.end : paramEnd(f.params[i - self]); };
    items("(", ")", toks_[open].span.end, toks_[close].span.begin, f.params.size() + self, begin, end,
          [&](std::size_t i) {
              if (i < self) {
                  flush(begin(i));
                  text("self");
              } else {
                  param(f.params[i - self]);
              }
          });
    if (f.result != nullptr) {
        space();
        text("->");
        space();
        type(f.result);
    }
    if (f.body != nullptr) {
        space();
        block(f.body);
    }
}

void Printer::structDecl(const StructDecl& s) {
    text(s.kind == DeclKind::State ? "state" : "struct");
    space();
    flushAfterToken(s.span.begin);
    text(name(s.name));
    typeParams(s.typeParams);
    if (src_[s.span.end - 1] != '}') {
        return;
    }
    space();
    text("{");
    std::uint32_t closeAt = s.span.end - 1;
    if (s.fields.empty() && !commentBefore(closeAt)) {
        text("}");
        return;
    }
    {
        Indent in(*this);
        for (const FieldDecl& f : s.fields) {
            std::uint32_t begin = f.attrs.empty() ? f.span.begin : f.attrs[0].span.begin;
            newline(begin);
            flush(begin);
            if (blankLineBefore(begin)) {
                blankLine();
            }
            Scope scope(*this);
            attributes(f.attrs);
            flush(f.span.begin);
            text(name(f.name));
            text(":");
            space();
            type(f.type);
            if (f.defaultValue != nullptr) {
                space();
                text("=");
                space();
                expr(f.defaultValue);
            }
        }
        newline(closeAt);
        flush(closeAt);
    }
    newline(closeAt);
    text("}");
}

void Printer::enumDecl(const EnumDecl& e) {
    text("enum");
    space();
    flushAfterToken(e.span.begin);
    text(name(e.name));
    typeParams(e.typeParams);
    space();
    text("{");
    std::uint32_t closeAt = e.span.end - 1;
    if (e.variants.empty() && !commentBefore(closeAt)) {
        text("}");
        return;
    }
    {
        Indent in(*this);
        for (const Variant& v : e.variants) {
            newline(v.span.begin);
            flush(v.span.begin);
            if (blankLineBefore(v.span.begin)) {
                blankLine();
            }
            Scope scope(*this);
            text(name(v.name));
            std::size_t open = tokenAt(v.span.begin) + 1;
            if (open >= toks_.size() || toks_[open].kind != Tok::LParen) {
                continue;
            }
            std::size_t close = matching(open);
            items(
                "(", ")", toks_[open].span.end, toks_[close].span.begin, v.payload.size(),
                [&](std::size_t i) { return v.payload[i]->span.begin; },
                [&](std::size_t i) { return v.payload[i]->span.end; }, [&](std::size_t i) { type(v.payload[i]); });
        }
        newline(closeAt);
        flush(closeAt);
    }
    newline(closeAt);
    text("}");
}

void Printer::type(const TypeExpr* t) {
    flush(t->span.begin);
    switch (t->kind) {
    case TypeExprKind::Named: {
        const auto* n = static_cast<const NamedTypeExpr*>(t);
        if (n->path.size() == 1 && name(n->path[0]) == "Self" && src_.substr(t->span.begin, 4) == "self") {
            text("self");
        } else {
            path(n->path);
        }
        if (!n->args.empty()) {
            typeArgs(n->args, t->span.end - 1);
        }
        return;
    }
    case TypeExprKind::List:
        text("[");
        type(static_cast<const ListTypeExpr*>(t)->elem);
        flush(t->span.end - 1);
        text("]");
        return;
    case TypeExprKind::Array: {
        const auto* a = static_cast<const ArrayTypeExpr*>(t);
        text("[");
        type(a->elem);
        text(";");
        space();
        expr(a->size);
        flush(t->span.end - 1);
        text("]");
        return;
    }
    case TypeExprKind::Tuple:
    case TypeExprKind::Fn: {
        bool isFn = t->kind == TypeExprKind::Fn;
        std::span<TypeExpr* const> elems =
            isFn ? static_cast<const FnTypeExpr*>(t)->params : static_cast<const TupleTypeExpr*>(t)->elems;
        std::size_t open = tokenAt(t->span.begin) + (isFn ? 1 : 0);
        std::size_t close = matching(open);
        if (isFn) {
            text("fn");
        }
        items(
            "(", ")", toks_[open].span.end, toks_[close].span.begin, elems.size(),
            [&](std::size_t i) { return elems[i]->span.begin; }, [&](std::size_t i) { return elems[i]->span.end; },
            [&](std::size_t i) { type(elems[i]); });
        if (isFn && static_cast<const FnTypeExpr*>(t)->result != nullptr) {
            space();
            text("->");
            space();
            type(static_cast<const FnTypeExpr*>(t)->result);
        }
        return;
    }
    case TypeExprKind::Nullable:
        type(static_cast<const NullableTypeExpr*>(t)->inner);
        text("?");
        return;
    }
}

void Printer::typeArgs(std::span<TypeExpr* const> args, std::uint32_t closeAt) {
    text("<");
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            text(",");
            space();
        }
        type(args[i]);
    }
    flush(closeAt);
    text(">");
}

} // namespace quartz::format
