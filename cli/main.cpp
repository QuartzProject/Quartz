#include "support/Diagnostics.hpp"
#include "syntax/Parser.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: quartz <file.qz>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    quartz::SourceManager sources;
    const auto& file = sources.add(argv[1], ss.str());
    quartz::Diagnostics diags(sources);
    quartz::Arena arena;
    quartz::Interner names;
    quartz::syntax::parseModule(file, arena, names, diags);
    std::fputs(diags.render().c_str(), stderr);
    return diags.hasErrors() ? 1 : 0;
}
