#include "ir/Lower.hpp"
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
    bool printIr = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--ir") {
            printIr = true;
            continue;
        }
        std::ifstream in(argv[i], std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto& file = sources.add(argv[i], ss.str());
        modules.push_back(quartz::syntax::parseModule(file, arena, names, diags));
    }
    if (!diags.hasErrors()) {
        auto prog = quartz::sema::analyze(modules, names, arena, diags);
        if (!diags.hasErrors()) {
            auto mod = quartz::ir::lower(*prog, sources, diags);
            if (printIr) {
                std::fputs(quartz::ir::print(*mod).c_str(), stdout);
            }
        }
    }
    std::fputs(diags.render().c_str(), stderr);
    return diags.hasErrors() ? 1 : 0;
}
