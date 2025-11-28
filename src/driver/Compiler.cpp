#include "quartz/Quartz.hpp"

#include "ir/Lower.hpp"
#include "ir/Optimize.hpp"
#include "ir/Validator.hpp"
#include "sema/Sema.hpp"
#include "syntax/Parser.hpp"

namespace quartz {

CompileOutput compile(std::span<const SourceInput> inputs, CompileOptions options) {
    CompileOutput out;
    SourceManager sources;
    Diagnostics diags(sources);
    Arena arena;
    Interner names;
    std::vector<syntax::Module*> modules;
    for (const SourceInput& in : inputs) {
        const SourceFile& file = sources.add(in.name, in.text);
        modules.push_back(syntax::parseModule(file, arena, names, diags));
    }
    if (diags.hasErrors()) {
        out.diagnostics = diags.render();
        return out;
    }
    auto prog = sema::analyze(modules, names, arena, diags);
    if (diags.hasErrors()) {
        out.diagnostics = diags.render();
        return out;
    }
    auto mod = ir::lower(*prog, sources, diags);
    out.diagnostics = diags.render();
    if (diags.hasErrors()) {
        return out;
    }
    if (options.optimize) {
        ir::optimize(*mod);
    }
    if (options.printIr) {
        out.irText = ir::print(*mod);
    }
    if (auto v = ir::validate(*mod); !v) {
        out.diagnostics += v.error().describe(*mod) + "\n";
        return out;
    }
    out.program = vm::compile(std::move(mod));
    return out;
}

} // namespace quartz
