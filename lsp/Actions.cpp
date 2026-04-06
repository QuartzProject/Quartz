#include "Imports.hpp"
#include "Server.hpp"
#include "Text.hpp"

namespace quartz::lsp {

namespace {

struct MissingName {
    std::string name;
    bool typeOnly = false;
};

// The name a diagnostic reports as unresolved, from sema's "unknown ..." messages and the math import hint.
std::optional<MissingName> missingName(std::string_view message) {
    bool unknown = message.starts_with("unknown function '") || message.starts_with("unknown name '") ||
                   message.starts_with("unknown type '");
    bool math = message.starts_with("'") && message.contains("' is in ");
    if (!unknown && !math) {
        return std::nullopt;
    }
    std::size_t open = message.find('\'');
    std::size_t close = message.find('\'', open + 1);
    if (close == std::string_view::npos || close == open + 1) {
        return std::nullopt;
    }
    std::string_view n = message.substr(open + 1, close - open - 1);
    if (n.find_first_of(".<>[]()?, ") != std::string_view::npos) {
        return std::nullopt;
    }
    return MissingName{std::string(n), message.starts_with("unknown type '")};
}

bool isType(ExportKind k) { return k == ExportKind::Struct || k == ExportKind::Enum || k == ExportKind::Trait; }

Json workspaceEdit(std::string_view uri, Json edit) {
    return Json::object().set("changes", Json::object().set(std::string(uri), Json::array().push(std::move(edit))));
}

} // namespace

Json Server::codeAction(const Json& params) {
    std::string uri(params["textDocument"]["uri"].str());
    Json out = Json::array();
    Analysis* a = analysis(uri);
    std::optional<std::uint32_t> file = a != nullptr ? a->fileOf(uri) : std::nullopt;
    const syntax::Module* m = file ? a->module(*file) : nullptr;
    if (m == nullptr) {
        return out;
    }
    ImportScope scope = importScope(*a, *m);
    std::vector<Export> all;
    const LineIndex& index = a->lines(*file);
    for (const Json& d : params["context"]["diagnostics"].items()) {
        std::optional<MissingName> missing = missingName(d["message"].str());
        if (!missing || !d["range"].isObject()) {
            continue;
        }
        if (all.empty()) {
            all = exports(*a);
        }
        std::vector<const Export*> found;
        for (const Export& e : all) {
            if (e.name == missing->name && e.package != scope.package && (!missing->typeOnly || isType(e.kind))) {
                found.push_back(&e);
            }
        }
        for (const Export* e : found) {
            Json action = Json::object().set("kind", "quickfix");
            if (std::string q = scope.qualifierOf(e->package); !q.empty()) {
                Json at = d["range"]["start"];
                Json edit =
                    Json::object().set("range", Json::object().set("start", at).set("end", at)).set("newText", q + ".");
                action.set("title", "Use '" + q + "." + e->name + "'").set("edit", workspaceEdit(uri, std::move(edit)));
            } else if (!scope.imports(e->package, e->name)) {
                TextEdit edit = importEdit(*a, *file, e->package, e->name);
                action.set("title", "Import '" + e->name + "' from " + e->package)
                    .set("edit", workspaceEdit(uri, textEdit(index, edit)));
            } else {
                continue;
            }
            action.set("diagnostics", Json::array().push(d)).set("isPreferred", found.size() == 1);
            out.push(std::move(action));
        }
    }
    return out;
}

} // namespace quartz::lsp
