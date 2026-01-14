#define DOCTEST_CONFIG_IMPLEMENT
#include "quartz/Quartz.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace {

namespace fs = std::filesystem;
using quartz::vm::Slot;
using quartz::vm::TierMode;

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::shared_ptr<const quartz::vm::Program> build(std::vector<quartz::SourceInput> inputs) {
    quartz::CompileOutput out = quartz::compile(inputs);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    return out.program;
}

struct Outcome {
    std::string log;
    std::string trap;

    bool operator==(const Outcome&) const = default;
};

bool logText(quartz::vm::HostCall& call, void* user) {
    auto* log = static_cast<std::string*>(user);
    *log += static_cast<const quartz::vm::Str*>(call.args[0].p)->view();
    *log += '\n';
    return true;
}

void requireNative(const quartz::vm::Program& p, std::uint32_t f) {
    INFO(p.functions[f].name, ": ", p.tier->error(f));
    REQUIRE(p.tier->entry(f) != nullptr);
}

Outcome runMain(const std::shared_ptr<const quartz::vm::Program>& p, TierMode mode, std::int64_t fuel = 100'000'000) {
    Outcome o;
    quartz::vm::Limits limits;
    limits.tier = mode;
    limits.fuelPerCall = fuel;
    quartz::vm::Sandbox sb(p, limits);
    sb.bind("print", logText, &o.log);
    REQUIRE(sb.start().has_value());
    std::uint32_t f = p->findFunction("main");
    auto r = sb.call(f, {});
    if (r) {
        o.log += std::format("result {}\n", r->i);
    } else {
        o.trap = r.error().describe();
    }
    if (mode == TierMode::Native) {
        requireNative(*p, f);
    }
    return o;
}

std::shared_ptr<const quartz::vm::Program> withStd(const std::string& name, const std::string& text) {
    return build({{name, text}, {"<std>.qzi", "fn print(text: str)\n"}});
}

} // namespace

TEST_CASE("native code prints and returns what the interpreter does") {
    for (const auto& e : fs::directory_iterator("tests/programs")) {
        if (e.path().extension() != ".qz") {
            continue;
        }
        CAPTURE(e.path().string());
        auto p = withStd(e.path().string(), readFile(e.path()));
        CHECK(runMain(p, TierMode::Native) == runMain(p, TierMode::Interpreter));
    }
}

TEST_CASE("native traps and fuel match the interpreter's code, message, function and line") {
    const char* bodies[] = {
        "let z = [0][0]\n    return 10 / z",
        "let xs = [1, 2, 3]\n    return xs[xs.len()]",
        "let m: Map<str, i64> = Map()\n    return m[\"nope\"]",
        "let s = \"x\"\n    panic(\"boom {s}\")\n    return 0",
        "var i = 0\n    while true { i += 1 }\n    return i",
        "return rec(0)",
        "return \"abc\".slice(1, 9).len()",
        "return \"abc\".byteAt(3)",
        "let ps = [(1, 2.0)]\n    ps[3] = (4, 5.0)\n    return 0",
        "let n = 9223372036854775807\n    var s = 0\n    for i in 0..n { s += i }\n    return s",
        "var j = 9223372036854775800\n    while j <= 9223372036854775806 { j += 3 }\n    return j",
        "var s = 0\n    for i in 0..100001 { s += i }\n    return s",
        "var s = 0\n    for i in 0..=100000 { s += i }\n    return s",
        "var s = 0\n    for i in 0..100000 { s += i }\n    return s",
    };
    for (const char* body : bodies) {
        CAPTURE(body);
        std::string text =
            std::format("fn rec(n: i64) -> i64 {{ return rec(n + 1) + 1 }}\nfn main() -> i64 {{\n    {}\n}}\n", body);
        auto p = withStd("trap.qz", text);
        Outcome native = runMain(p, TierMode::Native, 100'000);
        Outcome interp = runMain(p, TierMode::Interpreter, 100'000);
        CHECK(native == interp);
    }
}

namespace {

struct Host {
    std::string log;
};

std::int64_t pid(const Slot* a) { return static_cast<std::int64_t>(a[0].u); }

bool hId(quartz::vm::HostCall& c, void*) {
    c.result.i = pid(c.args);
    return true;
}

bool hLevel(quartz::vm::HostCall& c, void*) {
    c.result.i = pid(c.args) % 5;
    return true;
}

bool hName(quartz::vm::HostCall& c, void*) {
    c.result.p = c.string(std::format("Player{}", pid(c.args)));
    return c.result.p != nullptr;
}

bool hTrusted(quartz::vm::HostCall& c, void*) {
    c.result.u = (pid(c.args) * 7 + c.args[1].i * 13) % 5 == 0;
    return true;
}

bool hText(quartz::vm::HostCall& c, void* user) {
    auto* h = static_cast<Host*>(user);
    h->log += std::format("{} {}\n", pid(c.args), static_cast<const quartz::vm::Str*>(c.args[1].p)->view());
    return true;
}

std::string runPlugin(const std::shared_ptr<const quartz::vm::Program>& p, TierMode mode) {
    Host host;
    quartz::vm::Limits limits;
    limits.tier = mode;
    quartz::vm::Sandbox sb(p, limits);
    sb.bind("Player.id", hId);
    sb.bind("Player.level", hLevel);
    sb.bind("Player.name", hName);
    sb.bind("trusted", hTrusted);
    sb.bind("send", hText, &host);
    sb.bind("setScore", hText, &host);
    REQUIRE(sb.start().has_value());
    REQUIRE(sb.call(p->findFunction("init"), {}).has_value());
    std::uint64_t seed = 42;
    auto rnd = [&](std::uint64_t m) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::int64_t>((seed >> 33) % m);
    };
    const char* words[] = {"hello", "SPAM", "world", "hAcK", "ok", "nice build", "idiot"};
    for (int i = 0; i < 400; ++i) {
        Slot a[4];
        a[0].u = static_cast<std::uint64_t>(rnd(64));
        std::string event;
        std::expected<Slot, quartz::vm::Trap> r;
        switch (i % 5) {
        case 0:
            a[1].i = rnd(2000);
            a[2].i = rnd(200);
            a[3].i = rnd(2000);
            r = sb.call(p->findFunction("onBreak"), a);
            break;
        case 1:
            a[1].i = rnd(64);
            a[2].i = 1 + rnd(500);
            r = sb.call(p->findFunction("onTransfer"), std::span<const Slot>(a, 3));
            break;
        case 2:
            a[1].p = sb.newStr(std::format("{} {}", words[rnd(7)], words[rnd(7)]));
            a[2].i = i / 2;
            r = sb.call(p->findFunction("onChat"), std::span<const Slot>(a, 3));
            break;
        case 3:
            r = sb.call(p->findFunction("onTick"), {});
            break;
        default:
            r = sb.call(p->findFunction("onSieve"), {});
            break;
        }
        host.log += r ? std::format("= {}\n", r->i) : r.error().describe() + "\n";
    }
    if (mode == TierMode::Native) {
        for (const char* f : {"init", "onBreak", "onTransfer", "onChat", "onTick", "onSieve"}) {
            requireNative(*p, p->findFunction(f));
        }
    }
    return host.log;
}

} // namespace

TEST_CASE("native code runs the benchmark plugin like the interpreter") {
    auto p =
        build({{"host.qzi", readFile("bench/plugin/host.qzi")}, {"plugin.qz", readFile("bench/plugin/plugin.qz")}});
    std::string native = runPlugin(p, TierMode::Native);
    CHECK(native == runPlugin(p, TierMode::Interpreter));
}

int main(int argc, char** argv) {
    setenv("QUARTZ_COMPILER_LIB", QUARTZ_COMPILER_PATH, 1);
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
