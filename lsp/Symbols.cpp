#include "Imports.hpp"
#include "Server.hpp"

namespace quartz::lsp {

using namespace syntax;

namespace {

constexpr std::size_t kMaxSymbols = 500;

enum SymbolKind : int {
    kMethod = 6,
    kEnum = 10,
    kInterface = 11,
    kFunction = 12,
    kConstant = 14,
    kStruct = 23,
};

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Case-insensitive subsequence match, the usual fuzzy rule of workspace symbol search.
bool matches(std::string_view name, std::string_view query) {
    std::size_t at = 0;
    for (char c : name) {
        if (at < query.size() && lower(c) == lower(query[at])) {
            ++at;
        }
    }
    return at == query.size();
}

int symbolKind(const Decl& d) {
    switch (d.kind) {
    case DeclKind::Fn:
        return static_cast<const FnDecl&>(d).hasSelf ? kMethod : kFunction;
    case DeclKind::Enum:
        return kEnum;
    case DeclKind::Trait:
        return kInterface;
    case DeclKind::Const:
        return kConstant;
    default:
        return kStruct;
    }
}

} // namespace

Json Server::workspaceSymbols(const Json& params) {
    std::string_view query = params["query"].str();
    Json out = Json::array();
    std::size_t count = 0;
    for (const std::string& folder : workspace_.folders()) {
        bool stale = workspace_.isDirty(folder);
        Analysis* a = workspace_.analyzeFolder(folder);
        if (a == nullptr) {
            continue;
        }
        if (stale) {
            publish(folder, *a);
        }
        for (std::uint32_t f = 0; f < a->fileCount(); ++f) {
            const Module* m = a->module(f);
            if (m == nullptr || a->input(f).uri.empty()) {
                continue;
            }
            std::string package = joinPath(*a, m->package);
            auto add = [&](const Decl& d, const std::string& container) {
                if (!d.name.valid() || count >= kMaxSymbols || !matches(a->names().name(d.name), query)) {
                    return;
                }
                Json s = Json::object()
                             .set("name", a->names().name(d.name))
                             .set("kind", symbolKind(d))
                             .set("location", location(*a, d.nameSpan));
                if (!container.empty()) {
                    s.set("containerName", container);
                }
                out.push(std::move(s));
                ++count;
            };
            for (const Decl* d : m->decls) {
                if (d == nullptr) {
                    continue;
                }
                if (d->kind != DeclKind::Impl) {
                    add(*d, package);
                    continue;
                }
                const auto* impl = static_cast<const ImplDecl*>(d);
                std::string target;
                if (impl->target != nullptr && impl->target->span.end <= a->input(f).text.size()) {
                    Span t = impl->target->span;
                    target = std::string(a->input(f).text.substr(t.begin, t.end - t.begin));
                }
                for (const FnDecl* method : impl->methods) {
                    if (method != nullptr) {
                        add(*method, target);
                    }
                }
            }
        }
    }
    return out;
}

} // namespace quartz::lsp
