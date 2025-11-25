#include "sema/Sema.hpp"
#include "support/Diagnostics.hpp"
#include "syntax/Parser.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: quartz <file.qz>...\n");
        return 2;
    }
    quartz::SourceManager sources;
    quartz::Diagnostics diags(sources);
    quartz::Arena arena;
    quartz::Interner names;
    std::vector<quartz::syntax::Module*> modules;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto& file = sources.add(argv[i], ss.str());
        modules.push_back(quartz::syntax::parseModule(file, arena, names, diags));
    }
    if (!diags.hasErrors()) {
        quartz::sema::analyze(modules, names, arena, diags);
    }
    std::fputs(diags.render().c_str(), stderr);
    return diags.hasErrors() ? 1 : 0;
}
