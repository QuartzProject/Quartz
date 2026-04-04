#include "Builtins.hpp"
#include "Locate.hpp"
#include "Query.hpp"
#include "Server.hpp"

#include <format>

namespace quartz::lsp {

using namespace syntax;

namespace {

bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

struct Signature {
    std::string prefix; // `fn name(`
    std::vector<std::string> params;
    std::vector<std::string> names;
    std::string suffix; // `) -> R`
};

struct OpenCall {
    std::uint32_t paren = 0;    // offset of `(`
    std::uint32_t argStart = 0; // just after `(` or the last top-level comma
    std::uint32_t commas = 0;
};

// The innermost unclosed call parenthesis before `offset`, scanning the code from `from`.
std::optional<OpenCall> openCall(std::string_view text, std::uint32_t from, std::uint32_t offset) {
    struct Open {
        char bracket;
        OpenCall call;
    };

    std::vector<Open> stack;
    std::vector<std::size_t> interps; // stack depth at which each string interpolation began
    bool inString = false;
    std::uint32_t i = from;
    while (i < offset) {
        char c = text[i];
        if (inString) {
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == '"') {
                inString = false;
            } else if (c == '{') {
                interps.push_back(stack.size());
                inString = false;
            }
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < offset && text[i + 1] == '/') {
            while (i < offset && text[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '\'') {
            ++i;
            while (i < offset && text[i] != '\'' && text[i] != '\n') {
                i += text[i] == '\\' ? 2 : 1;
            }
        } else if (c == '(' || c == '[' || c == '{') {
            stack.push_back({c, {i, i + 1, 0}});
        } else if (c == ')' || c == ']' || c == '}') {
            if (c == '}' && !interps.empty() && interps.back() == stack.size()) {
                interps.pop_back();
                inString = true;
            } else if (!stack.empty()) {
                stack.pop_back();
            }
        } else if (c == ',' && !stack.empty()) {
            ++stack.back().call.commas;
            stack.back().call.argStart = i + 1;
        }
        ++i;
    }
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (it->bracket != '(') {
            return std::nullopt;
        }
        std::uint32_t before = it->call.paren;
        while (before > 0 && (text[before - 1] == ' ' || text[before - 1] == '\t')) {
            --before;
        }
        if (before > 0 && (isIdentChar(text[before - 1]) || text[before - 1] == '>')) {
            return it->call;
        }
    }
    return std::nullopt;
}

// Splits a built-in signature such as `pow(x: float, y: float) -> float` into its parts.
Signature parseBuiltin(std::string prefix, std::string_view sig) {
    Signature s;
    std::size_t open = sig.find('(');
    std::size_t close = sig.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        s.prefix = prefix + std::string(sig);
        return s;
    }
    s.prefix = prefix + std::string(sig.substr(0, open + 1));
    s.suffix = std::string(sig.substr(close));
    std::string_view list = sig.substr(open + 1, close - open - 1);
    while (!list.empty()) {
        std::size_t comma = list.find(", ");
        std::string_view p = list.substr(0, comma);
        s.params.emplace_back(p);
        s.names.emplace_back(p.substr(0, p.find(':')));
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 2);
    }
    return s;
}

Signature fromFunction(const Analysis& a, const sema::FnInfo& fn) {
    Signature s;
    std::string full = signature(a, fn);
    s.prefix = full.substr(0, full.find('(') + 1);
    std::size_t first = fn.hasSelf ? 1 : 0;
    for (std::size_t i = first; i < fn.params.size(); ++i) {
        std::string n = i < fn.paramNames.size() && fn.paramNames[i].valid()
                            ? std::string(a.names().name(fn.paramNames[i]))
                            : std::string("_");
        std::string p = n + ": " + a.typeName(fn.params[i]);
        const Expr* d = i < fn.defaults.size() ? fn.defaults[i] : nullptr;
        if (d != nullptr && d->span.file < a.fileCount() && d->span.end <= a.input(d->span.file).text.size()) {
            p += " = " + std::string(a.input(d->span.file).text.substr(d->span.begin, d->span.end - d->span.begin));
        }
        s.params.push_back(std::move(p));
        s.names.push_back(std::move(n));
    }
    s.suffix = ")";
    if (fn.result != ir::kVoid) {
        s.suffix += " -> " + a.typeName(fn.result);
    }
    return s;
}

std::optional<Signature> fromConstruct(const Analysis& a, std::uint32_t type) {
    const Decl* d = typeDecl(a, type);
    if (d == nullptr || d->kind == DeclKind::Enum) {
        return std::nullopt;
    }
    const auto* st = static_cast<const StructDecl*>(d);
    const ir::TypeInfo& info = a.program()->types.info(type);
    Signature s{a.typeName(type) + "(", {}, {}, ")"};
    for (std::size_t i = 0; i < st->fields.size(); ++i) {
        const FieldDecl& f = st->fields[i];
        std::string n = f.name.valid() ? std::string(a.names().name(f.name)) : std::string("_");
        std::string p = n + ": " + (i < info.fields.size() ? a.typeName(info.fields[i].type) : std::string("?"));
        if (f.defaultValue != nullptr && f.defaultValue->span.end <= a.input(f.defaultValue->span.file).text.size()) {
            Span v = f.defaultValue->span;
            p += " = " + std::string(a.input(v.file).text.substr(v.begin, v.end - v.begin));
        }
        s.params.push_back(std::move(p));
        s.names.push_back(std::move(n));
    }
    return s;
}

std::optional<Signature> fromVariant(const Analysis& a, std::uint32_t type, std::uint32_t variant) {
    const sema::Program* p = a.program();
    if (p == nullptr || type == 0 || type >= p->types.size() || variant >= p->types.info(type).variants.size()) {
        return std::nullopt;
    }
    const ir::VariantInfo& v = p->types.info(type).variants[variant];
    Signature s{std::format("{}.{}(", a.typeName(type), v.name), {}, {}, ")"};
    for (std::uint32_t t : v.payload) {
        s.params.push_back(a.typeName(t));
        s.names.emplace_back();
    }
    return s;
}

// The call whose callee ends at `calleeEnd`, as sema resolved it.
std::optional<Signature> resolveCall(const Analysis& a, const Module& m, std::uint32_t calleeEnd) {
    std::vector<Node> path = locate(m, calleeEnd > 0 ? calleeEnd - 1 : 0, true);
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        if (it->kind != NodeKind::Expr || static_cast<const Expr*>(it->ptr)->kind != ExprKind::Call) {
            continue;
        }
        const auto* c = static_cast<const CallExpr*>(it->ptr);
        if (c->callee == nullptr || c->callee->span.end != calleeEnd) {
            continue;
        }
        if (c->fn != nullptr) {
            return fromFunction(a, *c->fn);
        }
        if (c->callKind == CallKind::Construct) {
            return fromConstruct(a, c->type);
        }
        if (c->callKind == CallKind::Variant) {
            return fromVariant(a, c->type, c->variant);
        }
        std::string_view n;
        std::uint32_t recv = 0;
        if (c->callee->kind == ExprKind::Ident) {
            n = a.names().name(static_cast<const IdentExpr*>(c->callee)->name);
        } else if (c->callee->kind == ExprKind::Member) {
            const auto* mem = static_cast<const MemberExpr*>(c->callee);
            n = mem->name.valid() ? a.names().name(mem->name) : std::string_view{};
            recv = mem->object != nullptr ? mem->object->type : 0;
        }
        if (recv != 0) {
            if (auto b = findBuiltinMethod(a, recv, n)) {
                return parseBuiltin("fn " + a.typeName(recv) + ".", b->signature);
            }
        } else if (const BuiltinFunction* b = findBuiltinFunction(n)) {
            return parseBuiltin("fn ", b->signature);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::uint32_t utf16Length(std::string_view s) {
    std::uint32_t n = 0;
    for (char c : s) {
        auto u = static_cast<unsigned char>(c);
        if ((u & 0xC0) != 0x80) {
            n += u >= 0xF0 ? 2 : 1;
        }
    }
    return n;
}

} // namespace

Json Server::signatureHelp(const Json& params) {
    std::optional<Cursor> c = cursor(params);
    const Module* m = c ? c->analysis->module(c->file) : nullptr;
    if (m == nullptr) {
        return nullptr;
    }
    std::string_view text = c->analysis->input(c->file).text;
    std::uint32_t offset = std::min<std::uint32_t>(c->offset, static_cast<std::uint32_t>(text.size()));
    std::uint32_t from = 0;
    for (const Decl* d : m->decls) {
        if (d != nullptr && d->span.begin <= offset) {
            from = d->span.begin;
        }
    }
    std::optional<OpenCall> call = openCall(text, from, offset);
    if (!call) {
        return nullptr;
    }
    std::uint32_t calleeEnd = call->paren;
    while (calleeEnd > 0 && (text[calleeEnd - 1] == ' ' || text[calleeEnd - 1] == '\t')) {
        --calleeEnd;
    }
    std::optional<Signature> sig = resolveCall(*c->analysis, *m, calleeEnd);
    if (!sig) {
        return nullptr;
    }
    std::uint32_t active = call->commas;
    std::string_view arg = text.substr(call->argStart, offset - call->argStart);
    std::size_t lead = arg.find_first_not_of(" \t\n");
    if (lead != std::string_view::npos) {
        std::size_t end = lead;
        while (end < arg.size() && isIdentChar(arg[end])) {
            ++end;
        }
        std::size_t eq = arg.find_first_not_of(" \t", end);
        if (end > lead && eq != std::string_view::npos && arg[eq] == '=' &&
            (eq + 1 >= arg.size() || arg[eq + 1] != '=')) {
            std::string_view n = arg.substr(lead, end - lead);
            for (std::size_t i = 0; i < sig->names.size(); ++i) {
                if (sig->names[i] == n) {
                    active = static_cast<std::uint32_t>(i);
                }
            }
        }
    }
    std::string label = sig->prefix;
    Json parameters = Json::array();
    for (std::size_t i = 0; i < sig->params.size(); ++i) {
        if (i > 0) {
            label += ", ";
        }
        std::uint32_t begin = utf16Length(label);
        label += sig->params[i];
        parameters.push(Json::object().set("label", Json::array().push(begin).push(utf16Length(label))));
    }
    label += sig->suffix;
    Json info = Json::object()
                    .set("label", std::move(label))
                    .set("parameters", std::move(parameters))
                    .set("activeParameter", active);
    return Json::object()
        .set("signatures", Json::array().push(std::move(info)))
        .set("activeSignature", 0)
        .set("activeParameter", active);
}

} // namespace quartz::lsp
