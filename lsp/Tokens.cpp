#include "Index.hpp"
#include "Server.hpp"
#include "Text.hpp"

#include <charconv>

namespace quartz::lsp {

namespace {

constexpr std::string_view kTokenTypes[] = {"namespace", "type",          "struct",    "enum",
                                            "interface", "typeParameter", "parameter", "variable",
                                            "property",  "enumMember",    "function",  "method"};

constexpr std::string_view kTokenModifiers[] = {"declaration", "readonly", "defaultLibrary"};

enum TokenType : int {
    kStruct = 2,
    kEnum = 3,
    kInterface = 4,
    kParameter = 6,
    kVariable = 7,
    kProperty = 8,
    kEnumMember = 9,
    kFunction = 10,
    kMethod = 11,
};

constexpr int kDeclaration = 1;
constexpr int kReadonly = 2;
constexpr int kDefaultLibrary = 4;

int tokenType(EntityKind k) {
    switch (k) {
    case EntityKind::Local:
    case EntityKind::Const:
        return kVariable;
    case EntityKind::Param:
        return kParameter;
    case EntityKind::Function:
        return kFunction;
    case EntityKind::Method:
        return kMethod;
    case EntityKind::Struct:
        return kStruct;
    case EntityKind::Enum:
        return kEnum;
    case EntityKind::Trait:
        return kInterface;
    case EntityKind::Field:
        return kProperty;
    case EntityKind::Variant:
        return kEnumMember;
    }
    return kVariable;
}

} // namespace

Json semanticTokensLegend() {
    Json types = Json::array();
    for (std::string_view t : kTokenTypes) {
        types.push(t);
    }
    Json modifiers = Json::array();
    for (std::string_view m : kTokenModifiers) {
        modifiers.push(m);
    }
    return Json::object().set("tokenTypes", std::move(types)).set("tokenModifiers", std::move(modifiers));
}

Json Server::semanticTokens(const Json& params) {
    std::string uri(params["textDocument"]["uri"].str());
    std::string data = "[";
    Analysis* a = analysis(uri);
    std::optional<std::uint32_t> file = a != nullptr ? a->fileOf(uri) : std::nullopt;
    if (file) {
        const LineIndex& index = a->lines(*file);
        Position previous;
        std::uint32_t lastEnd = 0;
        for (const Occurrence& o : a->index().inFile(*file)) {
            if (o.span.begin < lastEnd) {
                continue;
            }
            Position begin = index.position(o.span.begin);
            Position end = index.position(o.span.end);
            if (end.line != begin.line || end.character <= begin.character) {
                continue;
            }
            int modifiers = (o.declaration ? kDeclaration : 0) | (o.kind == EntityKind::Const ? kReadonly : 0) |
                            (a->input(o.declFile).uri.empty() ? kDefaultLibrary : 0);
            std::uint32_t deltaLine = begin.line - previous.line;
            std::uint32_t deltaStart = deltaLine == 0 ? begin.character - previous.character : begin.character;
            std::uint32_t fields[] = {deltaLine, deltaStart, end.character - begin.character,
                                      static_cast<std::uint32_t>(tokenType(o.kind)),
                                      static_cast<std::uint32_t>(modifiers)};
            for (std::uint32_t v : fields) {
                if (data.size() > 1) {
                    data += ',';
                }
                char buf[16];
                auto r = std::to_chars(buf, buf + sizeof buf, v);
                data.append(buf, r.ptr);
            }
            previous = begin;
            lastEnd = o.span.end;
        }
    }
    data += ']';
    return Json::object().set("data", Json::raw(std::move(data)));
}

} // namespace quartz::lsp
