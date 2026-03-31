#include "Builtins.hpp"
#include "Index.hpp"
#include "Server.hpp"

#include <algorithm>
#include <set>

namespace quartz::lsp {

namespace {

bool isIdentStart(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }

bool validName(std::string_view n) {
    if (n.empty() || n == "_" || n == "self" || !isIdentStart(static_cast<unsigned char>(n.front()))) {
        return false;
    }
    for (char c : n) {
        auto u = static_cast<unsigned char>(c);
        if (!isIdentStart(u) && !(u >= '0' && u <= '9')) {
            return false;
        }
    }
    return std::ranges::find(keywords(), n) == keywords().end() &&
           std::ranges::find(builtinTypes(), n) == builtinTypes().end();
}

} // namespace

const Occurrence* Server::renameTarget(const Cursor& c) {
    const Index& index = c.analysis->index();
    const Occurrence* o = index.at(c.file, c.offset);
    if (o == nullptr) {
        return nullptr;
    }
    if (!index.declaredInUserFile(o->key)) {
        throw RequestFailed{"only names declared in the open folder can be renamed"};
    }
    if (!o->exact) {
        throw RequestFailed{"this name is an import alias; rename its declaration instead"};
    }
    return o;
}

Json Server::references(const Json& params) {
    Json out = Json::array();
    std::optional<Cursor> c = cursor(params);
    const Occurrence* o = c ? c->analysis->index().at(c->file, c->offset) : nullptr;
    if (o == nullptr) {
        return out;
    }
    bool withDeclaration = params["context"]["includeDeclaration"].boolean(true);
    for (const Occurrence* r : c->analysis->index().of(o->key)) {
        if (withDeclaration || !r->declaration) {
            out.push(location(*c->analysis, r->span));
        }
    }
    return out;
}

Json Server::prepareRename(const Json& params) {
    std::optional<Cursor> c = cursor(params);
    const Occurrence* o = c ? renameTarget(*c) : nullptr;
    if (o == nullptr) {
        return nullptr;
    }
    std::string_view text = c->analysis->input(c->file).text;
    std::string placeholder(text.substr(o->span.begin, o->span.end - o->span.begin));
    return Json::object().set("range", range(*c->analysis, o->span)).set("placeholder", std::move(placeholder));
}

Json Server::rename(const Json& params) {
    std::string newName(params["newName"].str());
    if (!validName(newName)) {
        throw RequestFailed{"'" + newName + "' is not a valid name"};
    }
    std::optional<Cursor> c = cursor(params);
    const Occurrence* o = c ? renameTarget(*c) : nullptr;
    if (o == nullptr) {
        return nullptr;
    }
    const Analysis& a = *c->analysis;
    std::map<std::string, Json> byUri;
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    for (const Occurrence* r : a.index().of(o->key)) {
        if (!r->exact || !seen.insert({r->span.file, r->span.begin}).second) {
            continue;
        }
        Json edit = Json::object().set("range", range(a, r->span)).set("newText", newName);
        auto [it, fresh] = byUri.try_emplace(uriOf(a, r->span.file), Json::array());
        it->second.push(std::move(edit));
    }
    Json changes = Json::object();
    for (auto& [uri, edits] : byUri) {
        changes.set(uri, std::move(edits));
    }
    return Json::object().set("changes", std::move(changes));
}

} // namespace quartz::lsp
