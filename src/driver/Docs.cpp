#include "driver/Docs.hpp"

#include "ir/Ir.hpp"
#include "sema/Sema.hpp"
#include "support/Arena.hpp"
#include "support/Diagnostics.hpp"
#include "syntax/DocComment.hpp"
#include "syntax/Lexer.hpp"

#include <string>
#include <unordered_map>

namespace quartz {

namespace {

using ir::TypeId;
using ir::TypeKind;
using syntax::Expr;
using syntax::ExprKind;

class DocCollector {
public:
    DocCollector(const sema::Program& prog, const SourceManager& sources, const ir::TypeTable& types)
        : prog_(prog), sources_(sources), types_(types) {}

    [[nodiscard]] std::string docAt(Span span) const {
        return span.file < sources_.size() ? syntax::docComment(sources_.file(span.file).text(), span.begin) : "";
    }

    // The default's source text when the client can evaluate it alone: a literal of the declared type.
    [[nodiscard]] std::string defaultText(const Expr* e, TypeId type) const {
        if (e == nullptr || !literalOf(e, type) || e->span.file >= sources_.size()) {
            return {};
        }
        std::string_view text = sources_.file(e->span.file).text();
        if (e->span.end > text.size() || e->span.begin >= e->span.end) {
            return {};
        }
        std::string out(text.substr(e->span.begin, e->span.end - e->span.begin));
        if (e->kind == ExprKind::Member) {
            const ir::TypeInfo& info = types_.info(stripNullable(type));
            out = info.name + out.substr(out.rfind('.'));
        }
        return interfaceLiteral(out) ? out : std::string();
    }

    [[nodiscard]] bool library(Span span) const {
        return span.file >= sources_.size() || sources_.file(span.file).name().starts_with("<std>/");
    }

    [[nodiscard]] std::string_view name(Symbol s) const {
        return prog_.names != nullptr ? prog_.names->name(s) : std::string_view{};
    }

private:
    [[nodiscard]] TypeId stripNullable(TypeId t) const {
        return types_.kind(t) == TypeKind::Nullable ? types_.info(t).elem : t;
    }

    [[nodiscard]] bool literalOf(const Expr* e, TypeId type) const {
        TypeKind kind = types_.kind(type);
        if (kind == TypeKind::Nullable && e->kind == ExprKind::NullLit) {
            return true;
        }
        type = stripNullable(type);
        kind = types_.kind(type);
        switch (e->kind) {
        case ExprKind::IntLit:
            return types_.isInteger(type) || types_.isFloat(type);
        case ExprKind::FloatLit:
            return types_.isFloat(type);
        case ExprKind::BoolLit:
            return kind == TypeKind::Bool;
        case ExprKind::CharLit:
            return kind == TypeKind::Char;
        case ExprKind::StringLit:
            return kind == TypeKind::Str;
        case ExprKind::Unary: {
            const auto* u = static_cast<const syntax::UnaryExpr*>(e);
            return u->op == syntax::UnaryOp::Neg && (types_.isSigned(type) || types_.isFloat(type)) &&
                   (u->operand->kind == ExprKind::IntLit || u->operand->kind == ExprKind::FloatLit) &&
                   literalOf(u->operand, type);
        }
        case ExprKind::ListLit: {
            const auto* l = static_cast<const syntax::ListLitExpr*>(e);
            if (kind != TypeKind::List) {
                return false;
            }
            for (const Expr* x : l->elems) {
                if (!literalOf(x, types_.info(type).elem)) {
                    return false;
                }
            }
            return true;
        }
        case ExprKind::TupleLit: {
            const auto* t = static_cast<const syntax::TupleLitExpr*>(e);
            if (kind != TypeKind::Tuple || t->elems.size() != types_.info(type).elems.size()) {
                return false;
            }
            for (std::size_t i = 0; i < t->elems.size(); ++i) {
                if (!literalOf(t->elems[i], types_.info(type).elems[i])) {
                    return false;
                }
            }
            return true;
        }
        case ExprKind::Member: {
            const auto* m = static_cast<const syntax::MemberExpr*>(e);
            if (kind != TypeKind::Enum || m->safe || m->object->kind != ExprKind::Ident) {
                return false;
            }
            const ir::TypeInfo& info = types_.info(type);
            std::string_view enumName = name(static_cast<const syntax::IdentExpr*>(m->object)->name);
            std::string_view shortName = std::string_view(info.name).substr(info.name.rfind('.') + 1);
            if (enumName != info.name && enumName != shortName) {
                return false;
            }
            for (const ir::VariantInfo& v : info.variants) {
                if (v.name == name(m->name)) {
                    return v.payload.empty();
                }
            }
            return false;
        }
        default:
            return false;
        }
    }

    const sema::Program& prog_;
    const SourceManager& sources_;
    const ir::TypeTable& types_;
};

bool hasAny(const ir::DeclDocs& d) {
    for (const std::string& s : d.defaults) {
        if (!s.empty()) {
            return true;
        }
    }
    for (const std::string& s : d.members) {
        if (!s.empty()) {
            return true;
        }
    }
    return !d.doc.empty();
}

} // namespace

void collectDocs(const sema::Program& prog, const SourceManager& sources, ir::Module& m) {
    DocCollector docs(prog, sources, m.types);
    std::unordered_map<std::string, const sema::FnInfo*> byName;
    for (const sema::FnInfo* fn : prog.functions) {
        if (fn->decl != nullptr && fn->owner == ir::kError && !docs.library(fn->decl->span)) {
            byName.emplace(fn->package + '\n' + fn->name, fn);
        }
    }
    m.functionDocs.clear();
    for (std::uint32_t i = 0; i < m.functions.size(); ++i) {
        const ir::Function& f = m.functions[i];
        auto it = f.exported ? byName.find(f.package + '\n' + f.name) : byName.end();
        if (it == byName.end() || it->second->decl->params.size() != f.paramCount) {
            continue;
        }
        const syntax::FnDecl& decl = *it->second->decl;
        ir::DeclDocs d;
        d.index = i;
        d.doc = docs.docAt(decl.span);
        for (std::uint32_t p = 0; p < f.paramCount; ++p) {
            d.defaults.push_back(docs.defaultText(decl.params[p].defaultValue, f.regs[p]));
        }
        if (hasAny(d)) {
            m.functionDocs.push_back(std::move(d));
        }
    }
    m.typeDocs.clear();
    for (TypeId t = ir::kFirstUserType; t < m.types.size(); ++t) {
        const ir::TypeInfo& ti = m.types.info(t);
        if ((ti.kind != TypeKind::Struct && ti.kind != TypeKind::Enum) || ti.genericBase != 0 ||
            !ti.typeParams.empty()) {
            continue;
        }
        ir::DeclDocs d;
        d.index = t;
        if (auto s = prog.structDecls.find(t); s != prog.structDecls.end() && !docs.library(s->second->span)) {
            const ir::TypeInfo& info = m.types.info(t);
            if (s->second->fields.size() != info.fields.size()) {
                continue;
            }
            d.doc = docs.docAt(s->second->span);
            for (std::size_t k = 0; k < info.fields.size(); ++k) {
                const syntax::FieldDecl& field = s->second->fields[k];
                d.members.push_back(docs.docAt(field.span));
                d.defaults.push_back(docs.defaultText(field.defaultValue, info.fields[k].type));
            }
        } else if (auto e = prog.enumDecls.find(t); e != prog.enumDecls.end() && !docs.library(e->second->span)) {
            if (e->second->variants.size() != m.types.info(t).variants.size()) {
                continue;
            }
            d.doc = docs.docAt(e->second->span);
            for (const syntax::Variant& v : e->second->variants) {
                d.members.push_back(docs.docAt(v.span));
            }
        }
        if (hasAny(d)) {
            m.typeDocs.push_back(std::move(d));
        }
    }
}

bool interfaceLiteral(std::string_view text) {
    if (text.empty() || text.size() > 4096 || text.find_first_of("\r\n") != std::string_view::npos) {
        return false;
    }
    SourceManager sources;
    Diagnostics diags(sources);
    Arena arena;
    const SourceFile& file = sources.add("<default>", std::string(text));
    std::vector<syntax::Token> tokens = syntax::tokenize(file, arena, diags);
    if (diags.hasErrors() || tokens.size() < 2) {
        return false;
    }
    int depth = 0;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        switch (tokens[i].kind) {
        case syntax::Tok::LParen:
        case syntax::Tok::LBracket:
            ++depth;
            break;
        case syntax::Tok::RParen:
        case syntax::Tok::RBracket:
            if (--depth < 0) {
                return false;
            }
            break;
        case syntax::Tok::Comma:
            if (depth == 0) {
                return false;
            }
            break;
        case syntax::Tok::Minus:
            if (tokens[i + 1].kind != syntax::Tok::Int && tokens[i + 1].kind != syntax::Tok::Float) {
                return false;
            }
            break;
        case syntax::Tok::Int:
        case syntax::Tok::Float:
        case syntax::Tok::Char:
        case syntax::Tok::String:
        case syntax::Tok::KwTrue:
        case syntax::Tok::KwFalse:
        case syntax::Tok::KwNull:
        case syntax::Tok::Ident:
        case syntax::Tok::Dot:
            break;
        default:
            return false;
        }
    }
    return depth == 0 && tokens.back().kind == syntax::Tok::Eof;
}

} // namespace quartz
