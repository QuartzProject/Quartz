#include "Query.hpp"
#include "Server.hpp"
#include "Text.hpp"
#include "format/Format.hpp"
#include "syntax/DocComment.hpp"

namespace quartz::lsp {

using namespace syntax;

namespace {

enum SymbolKind : int {
    kClass = 5,
    kMethod = 6,
    kField = 8,
    kEnum = 10,
    kInterface = 11,
    kFunction = 12,
    kConstant = 14,
    kEnumMember = 22,
    kStruct = 23,
};

std::string_view slice(std::string_view text, Span s) {
    if (s.begin > s.end || s.end > text.size()) {
        return {};
    }
    return text.substr(s.begin, s.end - s.begin);
}

Json lspPosition(Position p) { return Json::object().set("line", p.line).set("character", p.character); }

bool inside(Span inner, Span outer) { return inner.begin >= outer.begin && inner.end <= outer.end; }

class SymbolBuilder {
public:
    SymbolBuilder(const Analysis& a, std::uint32_t file) : a_(a), text_(a.input(file).text), index_(a.lines(file)) {}

    Json symbol(std::string name, int kind, Span range, Span selection, std::string detail = {}) {
        if (!inside(selection, range)) {
            selection = {range.file, range.begin, range.begin};
        }
        Json s = Json::object().set("name", std::move(name)).set("kind", kind);
        if (!detail.empty()) {
            s.set("detail", std::move(detail));
        }
        return s.set("range", spanJson(range)).set("selectionRange", spanJson(selection));
    }

    Json decl(const Decl* d) {
        std::string n = d->name.valid() ? std::string(a_.names().name(d->name)) : std::string{};
        switch (d->kind) {
        case DeclKind::Fn: {
            const auto* fn = static_cast<const FnDecl*>(d);
            return symbol(n, fn->hasSelf ? kMethod : kFunction, d->span, d->nameSpan,
                          fn->info != nullptr ? signature(a_, *fn->info) : std::string{});
        }
        case DeclKind::Struct:
        case DeclKind::State: {
            Json s = symbol(n, kStruct, d->span, d->nameSpan, d->kind == DeclKind::State ? "state" : "struct");
            Json children = Json::array();
            for (const FieldDecl& f : static_cast<const StructDecl*>(d)->fields) {
                if (!f.name.valid()) {
                    continue;
                }
                Span r = f.type != nullptr ? f.span.to(f.type->span) : f.span;
                children.push(symbol(std::string(a_.names().name(f.name)), kField, r, f.span,
                                     f.type != nullptr ? std::string(slice(text_, f.type->span)) : std::string{}));
            }
            return s.set("children", std::move(children));
        }
        case DeclKind::Enum: {
            Json s = symbol(n, kEnum, d->span, d->nameSpan);
            Json children = Json::array();
            for (const Variant& v : static_cast<const EnumDecl*>(d)->variants) {
                if (v.name.valid()) {
                    children.push(symbol(std::string(a_.names().name(v.name)), kEnumMember, v.span, v.span));
                }
            }
            return s.set("children", std::move(children));
        }
        case DeclKind::Impl:
        case DeclKind::Trait: {
            bool impl = d->kind == DeclKind::Impl;
            Span selection = d->nameSpan;
            std::span<FnDecl* const> methods;
            if (impl) {
                const auto* i = static_cast<const ImplDecl*>(d);
                Span target = i->target != nullptr ? i->target->span : d->span;
                n = "impl " + std::string(slice(text_, target));
                if (i->trait != nullptr) {
                    n = "impl " + std::string(slice(text_, i->trait->span)) + " for " + n.substr(5);
                }
                selection = target;
                methods = i->methods;
            } else {
                methods = static_cast<const TraitDecl*>(d)->methods;
            }
            Json s = symbol(n.empty() ? "trait" : n, impl ? kClass : kInterface, d->span, selection);
            Json children = Json::array();
            for (const FnDecl* m : methods) {
                if (m != nullptr && m->name.valid()) {
                    children.push(decl(m));
                }
            }
            return s.set("children", std::move(children));
        }
        case DeclKind::Const: {
            const auto* c = static_cast<const ConstDecl*>(d);
            return symbol(n, kConstant, d->span, d->nameSpan,
                          c->info != nullptr ? a_.typeName(c->info->type) : std::string{});
        }
        }
        return nullptr;
    }

private:
    Json spanJson(Span s) {
        auto pos = [](Position p) { return Json::object().set("line", p.line).set("character", p.character); };
        return Json::object().set("start", pos(index_.position(s.begin))).set("end", pos(index_.position(s.end)));
    }

    const Analysis& a_;
    std::string_view text_;
    const LineIndex& index_;
};

} // namespace

Json textEdit(const LineIndex& index, const TextEdit& e) {
    Json r = Json::object()
                 .set("start", lspPosition(index.position(e.begin)))
                 .set("end", lspPosition(index.position(e.end)));
    return Json::object().set("range", std::move(r)).set("newText", e.text);
}

std::optional<Server::Cursor> Server::cursor(const Json& params) {
    std::string uri(params["textDocument"]["uri"].str());
    Analysis* a = analysis(uri);
    std::optional<std::uint32_t> file = a != nullptr ? a->fileOf(uri) : std::nullopt;
    if (!file) {
        return std::nullopt;
    }
    const Json& p = params["position"];
    const LineIndex& index = a->lines(*file);
    std::uint32_t offset = index.offset(
        {static_cast<std::uint32_t>(p["line"].integer()), static_cast<std::uint32_t>(p["character"].integer())});
    return Cursor{a, *file, offset};
}

Json Server::hover(const Json& params) {
    std::optional<Cursor> c = cursor(params);
    std::optional<Resolved> r = c ? resolveAt(*c->analysis, c->file, c->offset) : std::nullopt;
    if (!r || r->hover.empty()) {
        return nullptr;
    }
    std::string text = "```quartz\n" + r->hover + "\n```";
    if (r->declaration && r->declaration->file < c->analysis->fileCount()) {
        std::string doc = syntax::docComment(c->analysis->input(r->declaration->file).text, r->declaration->begin);
        text += doc.empty() ? "" : "\n\n" + doc;
    }
    Json contents = Json::object().set("kind", "markdown").set("value", std::move(text));
    return Json::object().set("contents", std::move(contents)).set("range", range(*c->analysis, r->range));
}

Json Server::definition(const Json& params) {
    std::optional<Cursor> c = cursor(params);
    std::optional<Resolved> r = c ? resolveAt(*c->analysis, c->file, c->offset) : std::nullopt;
    if (!r || !r->declaration) {
        return nullptr;
    }
    return location(*c->analysis, *r->declaration);
}

Json Server::completion(const Json& params) {
    Json items = Json::array();
    std::optional<Cursor> c = cursor(params);
    if (!c) {
        return items;
    }
    const LineIndex& index = c->analysis->lines(c->file);
    for (Completion& item : completeAt(*c->analysis, c->file, c->offset)) {
        bool imported = !item.package.empty();
        Json j = Json::object().set("label", item.label).set("kind", item.kind);
        if (!item.detail.empty()) {
            j.set("detail", std::move(item.detail));
        }
        if (imported) {
            j.set("labelDetails", Json::object().set("description", item.package));
        }
        j.set("sortText", (imported ? "1" : "0") + item.label);
        if (!item.insertText.empty()) {
            j.set("insertText", std::move(item.insertText));
        }
        if (!item.edits.empty()) {
            Json edits = Json::array();
            for (const TextEdit& e : item.edits) {
                edits.push(textEdit(index, e));
            }
            j.set("additionalTextEdits", std::move(edits));
        }
        items.push(std::move(j));
    }
    return items;
}

Json Server::documentSymbols(const Json& params) {
    std::string uri(params["textDocument"]["uri"].str());
    Analysis* a = analysis(uri);
    Json out = Json::array();
    if (a == nullptr) {
        return out;
    }
    auto file = a->fileOf(uri);
    const Module* m = file ? a->module(*file) : nullptr;
    if (m == nullptr) {
        return out;
    }
    SymbolBuilder builder(*a, *file);
    for (const Decl* d : m->decls) {
        if (d != nullptr && (d->name.valid() || d->kind == DeclKind::Impl)) {
            out.push(builder.decl(d));
        }
    }
    return out;
}

Json Server::formatting(const Json& params) {
    std::string uri(params["textDocument"]["uri"].str());
    const Document* doc = workspace_.document(uri);
    Json edits = Json::array();
    if (doc == nullptr) {
        return edits;
    }
    auto formatted = format::formatSource(doc->path.empty() ? uri : doc->path, doc->text);
    if (!formatted) {
        log("formatting skipped: " + formatted.error().substr(0, formatted.error().find('\n')));
        return edits;
    }
    if (*formatted == doc->text) {
        return edits;
    }
    LineIndex index(doc->text);
    Position end = index.end();
    Json r = Json::object()
                 .set("start", Json::object().set("line", 0).set("character", 0))
                 .set("end", Json::object().set("line", end.line).set("character", end.character));
    edits.push(Json::object().set("range", std::move(r)).set("newText", std::move(*formatted)));
    return edits;
}

} // namespace quartz::lsp
