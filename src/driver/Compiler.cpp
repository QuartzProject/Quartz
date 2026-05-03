#include "ir/Lower.hpp"
#include "ir/Optimize.hpp"
#include "ir/Prune.hpp"
#include "ir/Validator.hpp"
#include "quartz/Quartz.hpp"
#include "sema/Sema.hpp"
#include "std/Std.hpp"
#include "syntax/Parser.hpp"

#include <algorithm>

namespace quartz {

namespace {

void dropAttribute(syntax::Module& m, Symbol name) {
    for (syntax::Decl* d : m.decls) {
        auto kept = std::remove_if(d->attrs.begin(), d->attrs.end(), [&](const auto& a) { return a.name == name; });
        d->attrs = d->attrs.first(static_cast<std::size_t>(kept - d->attrs.begin()));
    }
}

} // namespace

CompileOutput compile(std::span<const SourceInput> inputs, CompileOptions options) {
    CompileOutput out;
    SourceManager sources;
    Diagnostics diags(sources);
    Arena arena;
    Interner names;
    std::vector<syntax::Module*> modules;
    Symbol test = names.intern("test");
    for (const SourceInput& in : inputs) {
        const SourceFile& file = sources.add(in.name, in.text);
        modules.push_back(syntax::parseModule(file, arena, names, diags));
        if (in.dependency && modules.back() != nullptr) {
            dropAttribute(*modules.back(), test);
        }
    }
    for (const auto& [name, text] : stdlib::sources()) {
        const SourceFile& file = sources.add(std::string(name), std::string(text));
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
    ir::pruneLibrary(*mod, "<std>/", stdlib::isNative);
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
    mod->manifest = options.manifest;
    if (options.emitBinary) {
        out.binary = ir::serialize(*mod, {.lines = options.binaryLines});
    }
    out.program = vm::compile(std::move(mod));
    return out;
}

std::expected<std::shared_ptr<const vm::Program>, std::string> load(std::span<const std::uint8_t> bytes,
                                                                    const ir::ReadOptions& options) {
    auto mod = ir::deserialize(bytes, options);
    if (!mod) {
        return std::unexpected(mod.error());
    }
    if (auto v = ir::validate(**mod); !v) {
        return std::unexpected(v.error().describe(**mod));
    }
    return std::shared_ptr<const vm::Program>(vm::compile(std::move(*mod)));
}

} // namespace quartz
