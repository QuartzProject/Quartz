#include "quartz/Quartz.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kStdInterface = "fn print(text: str)\n";

bool hostPrint(quartz::vm::HostCall& call, void*) {
    std::string_view s = static_cast<const quartz::vm::Str*>(call.args[0].p)->view();
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    return true;
}

int usage() {
    std::fprintf(stderr, "usage: quartz check|ir|run|dis <file.qz|file.qzi>... [--entry name]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    std::string_view cmd = argv[1];
    std::vector<quartz::SourceInput> inputs;
    std::string entry = "main";
    bool wantStd = true;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
            entry = argv[++i];
            continue;
        }
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        inputs.push_back({argv[i], ss.str()});
    }
    if (wantStd) {
        inputs.push_back({"<std>.qzi", std::string(kStdInterface)});
    }
    quartz::CompileOptions opts;
    opts.printIr = cmd == "ir";
    quartz::CompileOutput out = quartz::compile(inputs, opts);
    std::fputs(out.diagnostics.c_str(), stderr);
    if (!out.program) {
        return 1;
    }
    if (cmd == "check") {
        return 0;
    }
    if (cmd == "ir") {
        std::fputs(out.irText.c_str(), stdout);
        return 0;
    }
    if (cmd == "dis") {
        for (std::uint32_t f = 0; f < out.program->functions.size(); ++f) {
            std::fputs(quartz::vm::disassemble(*out.program, f).c_str(), stdout);
        }
        return 0;
    }
    if (cmd != "run") {
        return usage();
    }
    quartz::vm::Sandbox sb(out.program);
    sb.bind("print", hostPrint);
    if (auto s = sb.start(); !s) {
        std::fprintf(stderr, "%s\n", s.error().describe().c_str());
        return 3;
    }
    std::uint32_t fn = out.program->findFunction(entry);
    if (fn == ~0U) {
        std::fprintf(stderr, "no function '%s'\n", entry.c_str());
        return 2;
    }
    auto r = sb.call(fn, {});
    if (!r) {
        std::fprintf(stderr, "%s\n", r.error().describe().c_str());
        return 3;
    }
    const auto& f = out.program->functions[fn];
    if (f.result == quartz::ir::kI64) {
        std::printf("%lld\n", static_cast<long long>(r->i));
    }
    return 0;
}
