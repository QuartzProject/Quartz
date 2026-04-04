#include "Server.hpp"
#include "Text.hpp"
#include "Walk.hpp"

namespace quartz::lsp {

using namespace syntax;

namespace {

constexpr int kTypeHint = 1;
constexpr int kParameterHint = 2;

bool isLiteral(const Expr* e) {
    if (e != nullptr && e->kind == ExprKind::Unary) {
        e = static_cast<const UnaryExpr*>(e)->operand;
    }
    if (e == nullptr) {
        return false;
    }
    switch (e->kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::NullLit:
    case ExprKind::CharLit:
    case ExprKind::StringLit:
        return true;
    default:
        return false;
    }
}

// Initializers whose type is already written in them: literals, casts, constructors and enum variants.
bool typeIsEvident(const Expr* e) {
    if (e == nullptr || isLiteral(e) || e->kind == ExprKind::Interp || e->kind == ExprKind::Cast) {
        return true;
    }
    if (e->kind == ExprKind::Call) {
        CallKind k = static_cast<const CallExpr*>(e)->callKind;
        return k == CallKind::Construct || k == CallKind::Variant;
    }
    return e->kind == ExprKind::Member && static_cast<const MemberExpr*>(e)->isVariant;
}

struct Hint {
    std::uint32_t offset;
    std::string label;
    int kind;
};

class HintCollector : public Walker {
public:
    HintCollector(const Analysis& a, std::uint32_t begin, std::uint32_t end) : Walker(a), begin_(begin), end_(end) {}

    std::vector<Hint> hints;

protected:
    void onStmt(const Stmt& s) override {
        if (s.kind != StmtKind::Let) {
            return;
        }
        const auto& l = static_cast<const LetStmt&>(s);
        if (l.type != nullptr || l.local == nullptr || l.local->type == ir::kError || typeIsEvident(l.init)) {
            return;
        }
        add(l.nameSpan.end, ": " + a_.typeName(l.local->type), kTypeHint);
    }

    void onExpr(const Expr& e, const Expr*) override {
        if (e.kind != ExprKind::Call) {
            return;
        }
        const auto& c = static_cast<const CallExpr&>(e);
        std::vector<std::string_view> names;
        if (c.fn != nullptr && c.fn->decl != nullptr) {
            for (std::size_t i = c.fn->hasSelf ? 1 : 0; i < c.fn->paramNames.size(); ++i) {
                names.push_back(c.fn->paramNames[i].valid() ? a_.names().name(c.fn->paramNames[i]) : "");
            }
        } else if (c.callKind == CallKind::Construct) {
            const sema::Program* p = a_.program();
            if (p == nullptr) {
                return;
            }
            auto it = p->structDecls.find(baseType(c.type));
            if (it == p->structDecls.end()) {
                return;
            }
            for (const FieldDecl& f : it->second->fields) {
                names.push_back(f.name.valid() ? a_.names().name(f.name) : "");
            }
        }
        if (names.size() < 2) {
            return;
        }
        for (std::size_t i = 0; i < c.args.size() && i < names.size(); ++i) {
            const Arg& arg = c.args[i];
            if (arg.name.valid()) {
                return;
            }
            if (isLiteral(arg.value) && !names[i].empty() && names[i] != "_") {
                add(arg.value->span.begin, std::string(names[i]) + ":", kParameterHint);
            }
        }
    }

private:
    std::uint32_t baseType(std::uint32_t t) const {
        const sema::Program* p = a_.program();
        if (p == nullptr || t == 0 || t >= p->types.size()) {
            return t;
        }
        std::uint32_t base = p->types.info(t).genericBase;
        return base != 0 ? base : t;
    }

    void add(std::uint32_t offset, std::string label, int kind) {
        if (offset >= begin_ && offset <= end_) {
            hints.push_back({offset, std::move(label), kind});
        }
    }

    std::uint32_t begin_;
    std::uint32_t end_;
};

} // namespace

Json Server::inlayHints(const Json& params) {
    std::string uri(params["textDocument"]["uri"].str());
    Json out = Json::array();
    Analysis* a = analysis(uri);
    std::optional<std::uint32_t> file = a != nullptr ? a->fileOf(uri) : std::nullopt;
    const Module* m = file ? a->module(*file) : nullptr;
    if (m == nullptr) {
        return out;
    }
    LineIndex index(a->input(*file).text);
    auto offset = [&](const Json& p, std::uint32_t fallback) {
        if (!p.isObject()) {
            return fallback;
        }
        return index.offset(
            {static_cast<std::uint32_t>(p["line"].integer()), static_cast<std::uint32_t>(p["character"].integer())});
    };
    const Json& r = params["range"];
    HintCollector hints(*a, offset(r["start"], 0), offset(r["end"], ~0U));
    hints.module(*m);
    for (const Hint& h : hints.hints) {
        Position p = index.position(h.offset);
        Json hint = Json::object()
                        .set("position", Json::object().set("line", p.line).set("character", p.character))
                        .set("label", h.label)
                        .set("kind", h.kind);
        if (h.kind == kParameterHint) {
            hint.set("paddingRight", true);
        }
        out.push(std::move(hint));
    }
    return out;
}

} // namespace quartz::lsp
