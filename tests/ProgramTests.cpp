#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "quartz/Quartz.hpp"

#include <doctest/doctest.h>
#include <expected>
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

constexpr std::string_view kStd = R"(fn print(text: str)
@handle
@awaitable
struct Delay {}
impl i64 {
    @property
    fn ticks(self) -> Delay
}
)";

std::vector<quartz::SourceInput> withInterface(const fs::path& path, const std::string& text) {
    std::vector<quartz::SourceInput> inputs{{path.string(), text}};
    fs::path iface = path;
    iface.replace_extension(".qzi");
    if (fs::exists(iface)) {
        inputs.push_back({iface.string(), readFile(iface)});
    }
    return inputs;
}

bool hostTicks(quartz::vm::HostCall& call, void*) {
    call.result.u = call.args[0].u;
    return true;
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
        std::vector<quartz::SourceInput> inputs = withInterface(path, text);
        inputs.push_back({"<std>.qzi", std::string(kStd)});
        quartz::CompileOutput out = quartz::compile(inputs);
        REQUIRE_MESSAGE(out.program, out.diagnostics);
        std::string printed;
        quartz::vm::Sandbox sb(out.program);
        sb.bind("print", capturePrint, &printed);
        sb.bind("i64.ticks", hostTicks);
        REQUIRE(sb.start().has_value());
        auto t = sb.spawn(out.program->findFunction("main"), {});
        while (t && t->kind != quartz::vm::TaskOutcome::Kind::Done) {
            t = sb.resume(t->task);
        }
        auto r = t ? std::expected<quartz::vm::Slot, quartz::vm::Trap>(t->value)
                   : std::expected<quartz::vm::Slot, quartz::vm::Trap>(std::unexpected(t.error()));
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
        quartz::CompileOutput out = quartz::compile(withInterface(path, text));
        CHECK_FALSE(out.program);
        for (const std::string& want : directives(text, "error")) {
            CHECK_MESSAGE(out.diagnostics.find(want) != std::string::npos, want, "\n", out.diagnostics);
        }
    }
}
