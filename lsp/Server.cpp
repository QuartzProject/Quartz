#include "Server.hpp"

#include "Text.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>

namespace quartz::lsp {

namespace {

namespace fs = std::filesystem;

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInternalError = -32603;
constexpr int kRequestFailed = -32803;
constexpr int kDebounceMillis = 150;

struct MethodNotFound {};

Json position(Position p) { return Json::object().set("line", p.line).set("character", p.character); }

fs::path stdCacheDir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    fs::path base;
    if (xdg != nullptr && *xdg != '\0') {
        base = xdg;
    } else if (home != nullptr && *home != '\0') {
        base = fs::path(home) / ".cache";
    } else {
        std::error_code ec;
        base = fs::temp_directory_path(ec);
    }
    return base / "quartz-lsp" / "std";
}

} // namespace

int Server::run() {
    while (!exit_) {
        if (!workspace_.dirtyFolders().empty() && !transport_.pending(kDebounceMillis)) {
            flushDirty();
        }
        std::optional<std::string> body = transport_.read();
        if (!body) {
            return shutdown_ ? 0 : 1;
        }
        dispatch(*body);
    }
    return shutdown_ ? 0 : 1;
}

void Server::dispatch(const std::string& body) {
    std::optional<Json> msg = Json::parse(body);
    if (!msg || !msg->isObject()) {
        replyError(nullptr, kParseError, "invalid JSON");
        return;
    }
    const Json& id = (*msg)["id"];
    std::string method(((*msg)["method"]).str());
    bool isRequest = msg->has("id");
    if (method.empty()) {
        if (isRequest && !msg->has("result") && !msg->has("error")) {
            replyError(id, kInvalidRequest, "missing method");
        }
        return; // responses to our own requests are not used
    }
    try {
        if (!isRequest) {
            notification(method, (*msg)["params"]);
            return;
        }
        if (shutdown_) {
            replyError(id, kInvalidRequest, "the server is shutting down");
            return;
        }
        reply(id, request(method, (*msg)["params"]));
    } catch (const RequestFailed& e) {
        replyError(id, kRequestFailed, e.message);
    } catch (const MethodNotFound&) {
        replyError(id, kMethodNotFound, "unsupported method " + method);
    } catch (const std::exception& e) {
        log("error in " + method + ": " + e.what());
        if (isRequest) {
            replyError(id, kInternalError, std::string("internal error: ") + e.what());
        }
    } catch (...) {
        log("error in " + method);
        if (isRequest) {
            replyError(id, kInternalError, "internal error");
        }
    }
}

Json Server::request(const std::string& method, const Json& params) {
    if (method == "initialize") {
        Json caps = Json::object();
        caps.set("textDocumentSync", Json::object().set("openClose", true).set("change", 1));
        caps.set("hoverProvider", true);
        caps.set("definitionProvider", true);
        caps.set("documentSymbolProvider", true);
        caps.set("documentFormattingProvider", true);
        caps.set("completionProvider", Json::object().set("triggerCharacters", Json::array().push(".")));
        caps.set("signatureHelpProvider", Json::object()
                                              .set("triggerCharacters", Json::array().push("(").push(","))
                                              .set("retriggerCharacters", Json::array().push("=")));
        caps.set("inlayHintProvider", true);
        caps.set("referencesProvider", true);
        caps.set("semanticTokensProvider", Json::object().set("legend", semanticTokensLegend()).set("full", true));
        caps.set("workspaceSymbolProvider", true);
        caps.set("renameProvider", Json::object().set("prepareProvider", true));
        caps.set("codeActionProvider", Json::object().set("codeActionKinds", Json::array().push("quickfix")));
        return Json::object()
            .set("capabilities", std::move(caps))
            .set("serverInfo", Json::object().set("name", "quartz-lsp").set("version", "0.1.0"));
    }
    if (method == "shutdown") {
        shutdown_ = true;
        return nullptr;
    }
    if (method == "textDocument/hover") {
        return hover(params);
    }
    if (method == "textDocument/definition") {
        return definition(params);
    }
    if (method == "textDocument/completion") {
        return completion(params);
    }
    if (method == "textDocument/documentSymbol") {
        return documentSymbols(params);
    }
    if (method == "textDocument/formatting") {
        return formatting(params);
    }
    if (method == "textDocument/codeAction") {
        return codeAction(params);
    }
    if (method == "textDocument/signatureHelp") {
        return signatureHelp(params);
    }
    if (method == "textDocument/inlayHint") {
        return inlayHints(params);
    }
    if (method == "textDocument/semanticTokens/full") {
        return semanticTokens(params);
    }
    if (method == "workspace/symbol") {
        return workspaceSymbols(params);
    }
    if (method == "textDocument/references") {
        return references(params);
    }
    if (method == "textDocument/prepareRename") {
        return prepareRename(params);
    }
    if (method == "textDocument/rename") {
        return rename(params);
    }
    throw MethodNotFound{};
}

void Server::notification(const std::string& method, const Json& params) {
    const Json& doc = params["textDocument"];
    std::string uri(doc["uri"].str());
    if (method == "exit") {
        exit_ = true;
    } else if (method == "textDocument/didOpen" && !uri.empty()) {
        workspace_.open(uri, std::string(doc["text"].str()), doc["version"].integer());
    } else if (method == "textDocument/didChange" && !uri.empty()) {
        didChange(params);
    } else if (method == "textDocument/didClose" && !uri.empty()) {
        std::string folder = workspace_.folderOf(uri);
        workspace_.close(uri);
        notify("textDocument/publishDiagnostics", Json::object().set("uri", uri).set("diagnostics", Json::array()));
        published_[folder].erase(uri);
    }
}

void Server::didChange(const Json& params) {
    const Json& doc = params["textDocument"];
    std::string uri(doc["uri"].str());
    const Document* current = workspace_.document(uri);
    std::string text = current != nullptr ? current->text : std::string{};
    const Json& changes = params["contentChanges"];
    for (const Json& change : changes.items()) {
        const Json& r = change["range"];
        if (!r.isObject()) {
            text = std::string(change["text"].str());
            continue;
        }
        LineIndex index(text);
        auto at = [&](const Json& p) {
            return index.offset({static_cast<std::uint32_t>(p["line"].integer()),
                                 static_cast<std::uint32_t>(p["character"].integer())});
        };
        std::uint32_t begin = at(r["start"]);
        std::uint32_t end = at(r["end"]);
        if (begin > end) {
            std::swap(begin, end);
        }
        text.replace(begin, end - begin, change["text"].str());
    }
    workspace_.change(uri, std::move(text), doc["version"].integer());
}

Analysis* Server::analysis(const std::string& uri) {
    std::string folder = workspace_.folderOf(uri);
    bool stale = workspace_.isDirty(folder);
    Analysis* a = workspace_.analysisFor(uri);
    if (a != nullptr && stale) {
        publish(folder, *a);
    }
    return a;
}

void Server::flushDirty() {
    for (const std::string& folder : workspace_.dirtyFolders()) {
        try {
            if (Analysis* a = workspace_.analyzeFolder(folder)) {
                publish(folder, *a);
            }
        } catch (const std::exception& e) {
            log("analysis failed: " + std::string(e.what()));
        }
    }
}

void Server::publish(const std::string& folder, const Analysis& a) {
    std::map<std::string, Json> byUri;
    std::vector<std::string> open = workspace_.documentsIn(folder);
    for (const std::string& uri : open) {
        byUri[uri] = Json::array();
    }
    for (const Diagnostic& d : a.diagnostics()) {
        Json item = Json::object();
        std::string message = d.message;
        std::string uri;
        if (d.span.file < a.fileCount() && !a.input(d.span.file).uri.empty()) {
            uri = a.input(d.span.file).uri;
            item.set("range", range(a, d.span));
        } else {
            if (open.empty()) {
                continue;
            }
            uri = open.front();
            if (d.span.file < a.fileCount()) {
                message = a.input(d.span.file).name + ": " + message;
            }
            item.set("range", Json::object().set("start", position({})).set("end", position({})));
        }
        int severity = d.severity == Severity::Error ? 1 : d.severity == Severity::Warning ? 2 : 3;
        item.set("severity", severity).set("source", "quartz").set("message", message);
        byUri[uri].push(std::move(item));
    }
    std::set<std::string>& before = published_[folder];
    for (const std::string& uri : before) {
        if (!byUri.contains(uri)) {
            byUri[uri] = Json::array();
        }
    }
    before.clear();
    for (auto& [uri, list] : byUri) {
        if (list.size() > 0) {
            before.insert(uri);
        }
        Json params = Json::object().set("uri", uri);
        if (const Document* d = workspace_.document(uri)) {
            params.set("version", d->version);
        }
        params.set("diagnostics", std::move(list));
        notify("textDocument/publishDiagnostics", std::move(params));
    }
    log("analyzed " + folder + " in " + std::to_string(a.millis()) + " ms");
}

std::string Server::uriOf(const Analysis& a, std::uint32_t file) {
    const Analysis::Input& in = a.input(file);
    if (!in.uri.empty()) {
        return in.uri;
    }
    auto cached = stdUris_.find(in.name);
    if (cached != stdUris_.end()) {
        return cached->second;
    }
    std::string rel = in.name;
    if (rel.starts_with("<std>/")) {
        rel = "std/" + rel.substr(6);
    } else if (rel.starts_with("<std>")) {
        rel = "host" + rel.substr(5);
    }
    fs::path path = stdCacheDir() / rel;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << in.text;
    std::string uri = pathToUri(path.string());
    stdUris_[in.name] = uri;
    return uri;
}

Json Server::range(const Analysis& a, Span span) {
    if (span.file >= a.fileCount()) {
        return Json::object().set("start", position({})).set("end", position({}));
    }
    LineIndex index(a.input(span.file).text);
    std::uint32_t end = span.end > span.begin ? span.end : span.begin;
    return Json::object().set("start", position(index.position(span.begin))).set("end", position(index.position(end)));
}

Json Server::location(const Analysis& a, Span span) {
    if (span.file >= a.fileCount()) {
        return nullptr;
    }
    return Json::object().set("uri", uriOf(a, span.file)).set("range", range(a, span));
}

void Server::reply(const Json& id, Json result) {
    Json msg = Json::object().set("jsonrpc", "2.0").set("id", id).set("result", std::move(result));
    transport_.write(msg.dump());
}

void Server::replyError(const Json& id, int code, const std::string& message) {
    Json err = Json::object().set("code", code).set("message", message);
    transport_.write(Json::object().set("jsonrpc", "2.0").set("id", id).set("error", std::move(err)).dump());
}

void Server::notify(const std::string& method, Json params) {
    transport_.write(
        Json::object().set("jsonrpc", "2.0").set("method", method).set("params", std::move(params)).dump());
}

void Server::log(const std::string& message) {
    std::fprintf(stderr, "quartz-lsp: %s\n", message.c_str());
    std::fflush(stderr);
}

} // namespace quartz::lsp
