#pragma once

#include "Json.hpp"
#include "Transport.hpp"
#include "Workspace.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>

namespace quartz::lsp {

class LineIndex;
struct TextEdit;

/// The token types and modifiers `semanticTokens` encodes with.
Json semanticTokensLegend();

/// An LSP `TextEdit` for byte offsets of the text behind `index`.
Json textEdit(const LineIndex& index, const TextEdit& e);

struct Occurrence;

/// Thrown by a handler to answer its request with a RequestFailed error and this message.
struct RequestFailed {
    std::string message;
};

class Server {
public:
    /// Serves until `exit` or end of input; returns the process exit code.
    int run();

private:
    void dispatch(const std::string& body);
    Json request(const std::string& method, const Json& params);
    void notification(const std::string& method, const Json& params);
    void reply(const Json& id, Json result);
    void replyError(const Json& id, int code, const std::string& message);
    void notify(const std::string& method, Json params);
    void log(const std::string& message);

    Analysis* analysis(const std::string& uri);
    void publish(const std::string& folder, const Analysis& a);
    void flushDirty();
    std::string uriOf(const Analysis& a, std::uint32_t file);
    Json location(const Analysis& a, Span span);
    Json range(const Analysis& a, Span span);

    struct Cursor {
        Analysis* analysis;
        std::uint32_t file;
        std::uint32_t offset;
    };

    std::optional<Cursor> cursor(const Json& params);
    void didChange(const Json& params);
    Json hover(const Json& params);
    Json definition(const Json& params);
    Json completion(const Json& params);
    Json documentSymbols(const Json& params);
    Json formatting(const Json& params);
    Json codeAction(const Json& params);
    const Occurrence* renameTarget(const Cursor& c);
    Json references(const Json& params);
    Json prepareRename(const Json& params);
    Json rename(const Json& params);
    Json signatureHelp(const Json& params);
    Json inlayHints(const Json& params);
    Json semanticTokens(const Json& params);
    Json workspaceSymbols(const Json& params);

    Transport transport_;
    Workspace workspace_;
    std::map<std::string, std::set<std::string>> published_; // folder -> URIs holding diagnostics
    std::map<std::string, std::string> stdUris_;
    bool shutdown_ = false;
    bool exit_ = false;
};

} // namespace quartz::lsp
