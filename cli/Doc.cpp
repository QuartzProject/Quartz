#include "Commands.hpp"
#include "support/Arena.hpp"
#include "support/Diagnostics.hpp"
#include "support/Interner.hpp"
#include "syntax/DocComment.hpp"
#include "syntax/Parser.hpp"

#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <string>

namespace {

using namespace quartz;

std::string_view slice(std::string_view text, std::uint32_t begin, std::uint32_t end) {
    return end > begin && end <= text.size() ? text.substr(begin, end - begin) : std::string_view{};
}

std::string trimEnd(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '{' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

// What a reader needs: a function's header without its body; a type, trait or constant in full.
std::string signature(std::string_view text, const syntax::Decl* d) {
    if (d->kind == syntax::DeclKind::Fn) {
        const auto* fn = static_cast<const syntax::FnDecl*>(d);
        return trimEnd(slice(text, d->span.begin, fn->body != nullptr ? fn->body->span.begin : d->span.end));
    }
    return std::string(slice(text, d->span.begin, d->span.end));
}

std::string entry(std::string_view text, const syntax::Decl* d, std::string_view heading) {
    std::string attrs;
    for (const syntax::Attribute& a : d->attrs) {
        attrs += std::string(slice(text, a.span.begin, a.span.end)) + "\n";
    }
    std::string out = std::format("\n### {}\n\n```quartz\n{}{}\n```\n", heading, attrs, signature(text, d));
    std::string doc = syntax::docComment(text, d->span.begin);
    return doc.empty() ? out : out + "\n" + doc + "\n";
}

} // namespace

int runDoc(const std::vector<quartz::SourceInput>& inputs, const std::string& output) {
    std::map<std::string, std::string> packages;
    for (const quartz::SourceInput& in : inputs) {
        SourceManager sources;
        Diagnostics diags(sources);
        Arena arena;
        Interner names;
        const SourceFile& file = sources.add(in.name, in.text);
        syntax::Module* m = syntax::parseModule(file, arena, names, diags);
        if (m == nullptr) {
            continue;
        }
        std::string pkg;
        for (Symbol s : m->package) {
            pkg += (pkg.empty() ? "" : ".") + std::string(names.name(s));
        }
        std::string& out = packages[pkg.empty() ? "(no package)" : pkg];
        bool everything = file.isInterface();
        for (const syntax::Decl* d : m->decls) {
            std::string_view name = names.name(d->name);
            if (d->kind == syntax::DeclKind::Impl) {
                const auto* impl = static_cast<const syntax::ImplDecl*>(d);
                for (const syntax::FnDecl* fn : impl->methods) {
                    if (everything || fn->visibility == syntax::Visibility::Pub) {
                        std::string target(slice(in.text, impl->target->span.begin, impl->target->span.end));
                        out += entry(in.text, fn, target + "." + std::string(names.name(fn->name)));
                    }
                }
                continue;
            }
            if (everything || d->visibility == syntax::Visibility::Pub) {
                out += entry(in.text, d, name);
            }
        }
    }
    std::string doc;
    for (const auto& [pkg, body] : packages) {
        if (!body.empty()) {
            doc += std::format("{}## {}\n{}", doc.empty() ? "" : "\n", pkg, body);
        }
    }
    if (output.empty()) {
        std::fputs(doc.c_str(), stdout);
        return 0;
    }
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    file << doc;
    if (!file) {
        std::fprintf(stderr, "cannot write %s\n", output.c_str());
        return 2;
    }
    return 0;
}
