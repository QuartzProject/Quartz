#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "quartz/Quartz.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> directives(const std::string& text, std::string_view key) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    std::string prefix = "// " + std::string(key) + ":";
    while (std::getline(in, line)) {
        if (line.starts_with(prefix)) {
            std::string v = line.substr(prefix.size());
            out.push_back(v.empty() ? v : v.substr(1));
        }
    }
    return out;
}

bool capturePrint(quartz::vm::HostCall& call, void* user) {
    auto* out = static_cast<std::string*>(user);
    *out += static_cast<const quartz::vm::Str*>(call.args[0].p)->view();
    *out += '\n';
    return true;
}

std::vector<fs::path> files(const char* dir) {
    std::vector<fs::path> out;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".qz") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("programs print what their expect lines say") {
    for (const fs::path& path : files("tests/programs")) {
        CAPTURE(path.string());
        std::string text = readFile(path);
        quartz::SourceInput inputs[] = {{path.string(), text}, {"<std>.qzi", "fn print(text: str)\n"}};
        quartz::CompileOutput out = quartz::compile(inputs);
        REQUIRE_MESSAGE(out.program, out.diagnostics);
        std::string printed;
        quartz::vm::Sandbox sb(out.program);
        sb.bind("print", capturePrint, &printed);
        REQUIRE(sb.start().has_value());
        auto r = sb.call(out.program->findFunction("main"), {});
        std::vector<std::string> traps = directives(text, "trap");
        if (!traps.empty()) {
            REQUIRE_FALSE(r.has_value());
            CHECK(r.error().describe().find(traps[0]) != std::string::npos);
        } else {
            REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
        }
        std::string expected;
        for (const std::string& line : directives(text, "expect")) {
            expected += line + "\n";
        }
        CHECK(printed == expected);
    }
}

TEST_CASE("invalid programs report the expected diagnostics") {
    for (const fs::path& path : files("tests/diagnostics")) {
        CAPTURE(path.string());
        std::string text = readFile(path);
        quartz::SourceInput inputs[] = {{path.string(), text}};
        quartz::CompileOutput out = quartz::compile(inputs);
        CHECK_FALSE(out.program);
        for (const std::string& want : directives(text, "error")) {
            CHECK_MESSAGE(out.diagnostics.find(want) != std::string::npos, want, "\n", out.diagnostics);
        }
    }
}
