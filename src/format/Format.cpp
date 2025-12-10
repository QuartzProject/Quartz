#include "format/Format.hpp"

#include "format/Printer.hpp"
#include "support/Diagnostics.hpp"
#include "syntax/Lexer.hpp"
#include "syntax/Parser.hpp"

namespace quartz::format {

namespace {

struct Parsed {
    SourceManager sources;
    Diagnostics diags{sources};
    Arena arena;
    Interner names;
    const SourceFile* file = nullptr;
    syntax::Module* module = nullptr;
};

bool parse(Parsed& p, std::string_view name, std::string_view text) {
    p.file = &p.sources.add(std::string(name), std::string(text));
    p.module = syntax::parseModule(*p.file, p.arena, p.names, p.diags);
    return !p.diags.hasErrors();
}

} // namespace

std::expected<std::string, std::string> dumpSource(std::string_view name, std::string_view text) {
    Parsed p;
    if (!parse(p, name, text)) {
        return std::unexpected(p.diags.render());
    }
    return dumpModule(*p.module, p.names);
}

std::expected<std::string, std::string> formatSource(std::string_view name, std::string_view text) {
    Parsed p;
    if (!parse(p, name, text)) {
        return std::unexpected(p.diags.render());
    }
    std::vector<Span> comments;
    Diagnostics lexDiags(p.sources);
    std::vector<syntax::Token> toks = syntax::tokenize(*p.file, p.arena, lexDiags, &comments);
    std::string out = Printer(p.file->text(), toks, comments, p.names).run(*p.module);
    auto again = dumpSource(name, out);
    if (!again || *again != dumpModule(*p.module, p.names)) {
        return std::unexpected(std::string(name) + ": formatting would change the program; file left untouched\n");
    }
    return out;
}

} // namespace quartz::format
