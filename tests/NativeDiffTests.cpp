#define DOCTEST_CONFIG_IMPLEMENT
#include "quartz/Quartz.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>

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

bool forcedUnavailable() {
    const char* v = std::getenv("QUARTZ_NATIVE_LINKER");
    return v != nullptr && std::string_view(v) == "none";
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

// Same standard interface and file splitting as ProgramTests.cpp.
constexpr std::string_view kStd = R"(fn print(text: str)
@handle
@awaitable
struct Delay {}
impl int {
    @property
    fn ticks(self) -> Delay
}
)";

std::vector<quartz::SourceInput> programInputs(const fs::path& path) {
    std::vector<quartz::SourceInput> inputs{{path.string(), ""}};
    std::istringstream in(readFile(path));
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("// file: ")) {
            inputs.push_back({line.substr(9), ""});
            continue;
        }
        inputs.back().text += line + "\n";
    }
    fs::path iface = path;
    iface.replace_extension(".qzi");
    if (fs::exists(iface)) {
        inputs.push_back({iface.string(), readFile(iface)});
    }
    inputs.push_back({"<std>.qzi", std::string(kStd)});
    return inputs;
}

bool hostTicks(quartz::vm::HostCall& call, void*) {
    call.result.u = call.args[0].u;
    return true;
}

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
    sb.bind("int.ticks", hostTicks);
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
    return build({{name, text}, {"<std>.qzi", std::string(kStd)}});
}

} // namespace

// Every suspension is logged with its kind and handle, so the sequences must match too.
Outcome runTask(const std::shared_ptr<const quartz::vm::Program>& p, TierMode mode, std::int64_t fuel) {
    Outcome o;
    quartz::vm::Limits limits;
    limits.tier = mode;
    limits.fuelPerCall = fuel;
    quartz::vm::Sandbox sb(p, limits);
    sb.bind("print", logText, &o.log);
    sb.bind("int.ticks", hostTicks);
    sb.bind("ask", hostTicks);
    REQUIRE(sb.start().has_value());
    std::uint32_t f = p->findFunction("main");
    auto t = sb.spawn(f, {});
    bool migrated = false;
    for (int steps = 0; t && t->kind != quartz::vm::TaskOutcome::Kind::Done && steps < 100'000; ++steps) {
        o.log += std::format("[{} {}]\n", static_cast<int>(t->kind), t->value.u);
        if (mode == TierMode::Auto) {
            for (int i = 0; i < 2000 && p->tier->entry(f) == nullptr; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            migrated = migrated || sb.taskNative(t->task);
        }
        Slot v;
        v.u = t->value.u * 10 + 1;
        t = sb.resume(t->task, v);
    }
    if (t) {
        o.log += std::format("result {}\n", t->value.i);
    } else {
        o.trap = t.error().describe();
    }
    if (mode == TierMode::Native) {
        requireNative(*p, f);
    }
    if (mode == TierMode::Auto) {
        CHECK(migrated);
    }
    return o;
}

constexpr std::string_view kAsk = R"(@handle
@awaitable(int)
struct Ask {}

fn ask(n: int) -> Ask
)";

constexpr std::string_view kTaskProgram = R"(state Hub {
    names: [str]
    total: int
}

fn work(n: int) -> int {
    var s = 0
    for i in 0..n {
        s += i % 7
    }
    return s
}

suspend fn child(hub: Hub, k: int) -> str {
    var acc = ""
    var i = 0
    while i < k {
        let v = wait ask(i)
        acc = acc + "{v},"
        hub.total += work(50 * k)
        i += 1
    }
    var spin = 0
    while spin < 300 {
        spin += 1
    }
    return acc
}

suspend fn main(hub: Hub) -> int {
    hub.names.push("a")
    let first = hub.names[0]
    let r = child(hub, 3)
    hub.names[0] = "renamed"
    at 2.ticks {
        print("inside {first}")
    }
    var n = 0
    every 1.ticks {
        scope {
            let t = "n{n}"
            hub.names.push(t)
        }
        n += 1
        if n == 4 {
            break
        }
    }
    print("{r} {hub.total} {hub.names.join("-")} {first} {work(BIG)}")
    return n
}
)";

TEST_CASE("native tasks suspend, yield and print like the interpreter") {
    for (const auto& e : fs::directory_iterator("tests/programs")) {
        if (e.path().extension() != ".qz") {
            continue;
        }
        auto p = build(programInputs(e.path()));
        if (!p->functions[p->findFunction("main")].isSuspend) {
            continue;
        }
        for (std::int64_t fuel : {100'000'000, 40, 97, 1'000}) {
            CAPTURE(e.path().string());
            CAPTURE(fuel);
            CHECK(runTask(p, TierMode::Native, fuel) == runTask(p, TierMode::Interpreter, fuel));
        }
    }
    for (std::string big : {"10", "3000"}) {
        std::string text(kTaskProgram);
        text.replace(text.find("BIG"), 3, big);
        auto p = build({{"task.qz", text}, {"ask.qzi", std::string(kAsk)}, {"<std>.qzi", std::string(kStd)}});
        for (std::int64_t fuel : {100'000'000, 60, 250, 1'000, 2'500}) {
            CAPTURE(big);
            CAPTURE(fuel);
            Outcome native = runTask(p, TierMode::Native, fuel);
            Outcome interp = runTask(p, TierMode::Interpreter, fuel);
            CHECK(native.log == interp.log);
            CHECK(native.trap == interp.trap);
        }
    }
}

TEST_CASE("interpreted tasks move to native code at a wait and behave the same") {
    std::string text(kTaskProgram);
    text.replace(text.find("BIG"), 3, "10");
    auto p = build({{"task.qz", text}, {"ask.qzi", std::string(kAsk)}, {"<std>.qzi", std::string(kStd)}});
    Outcome interp = runTask(p, TierMode::Interpreter, 100'000'000);
    Outcome moved = runTask(p, TierMode::Auto, 100'000'000);
    CHECK(moved.log == interp.log);
    CHECK(moved.trap == interp.trap);
}

TEST_CASE("native code prints and returns what the interpreter does") {
    for (const auto& e : fs::directory_iterator("tests/programs")) {
        if (e.path().extension() != ".qz") {
            continue;
        }
        CAPTURE(e.path().string());
        auto p = build(programInputs(e.path()));
        if (p->functions[p->findFunction("main")].isSuspend) {
            continue; // tasks always run in the interpreter
        }
        CHECK(runMain(p, TierMode::Native) == runMain(p, TierMode::Interpreter));
    }
}

TEST_CASE("native traps and fuel match the interpreter's code, message, function and line") {
    const char* bodies[] = {
        "let z = [0][0]\n    return 10 / z",
        "let xs = [1, 2, 3]\n    return xs[xs.len()]",
        "let m: Map<str, int> = Map()\n    return m[\"nope\"]",
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
        "var s = 0\n    for i in 0..100000 {\n        s += i % 7\n    }\n    return s",
        "var s = 0\n    var i = 0\n    while i < 100000 {\n        s += i % 7\n        i += 1\n    }\n    return s",
    };
    for (const char* body : bodies) {
        CAPTURE(body);
        std::string text =
            std::format("fn rec(n: int) -> int {{ return rec(n + 1) + 1 }}\nfn main() -> int {{\n    {}\n}}\n", body);
        auto p = withStd("trap.qz", text);
        Outcome native = runMain(p, TierMode::Native, 100'000);
        Outcome interp = runMain(p, TierMode::Interpreter, 100'000);
        CHECK(native.log == interp.log);
        CHECK(native.trap == interp.trap);
    }
}

TEST_CASE("fuel charged by host functions inside a counted loop matches the interpreter") {
    std::string text = "import std.data.*\n"
                       "fn main() -> int {\n"
                       "    var n = 0\n"
                       "    for i in 0..300 {\n"
                       "        match parseJson(\"[1, 2, 3, 4, 5, 6, 7, 8]\") {\n"
                       "            Ok(v) => n += 1\n"
                       "            Err(e) => n -= 1\n"
                       "        }\n"
                       "    }\n"
                       "    return n\n"
                       "}\n";
    auto p = withStd("charge.qz", text);
    for (std::int64_t fuel : {1'000, 2'399, 2'400, 2'401, 100'000}) {
        CAPTURE(fuel);
        CHECK(runMain(p, TierMode::Native, fuel) == runMain(p, TierMode::Interpreter, fuel));
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

std::string runPlugin(const std::shared_ptr<const quartz::vm::Program>& p, TierMode mode,
                      const std::string& cacheDir = {}) {
    Host host;
    quartz::vm::Limits limits;
    limits.tier = mode;
    limits.nativeCacheDir = cacheDir;
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

TEST_CASE("cached native objects are reused, and ignored when damaged") {
    fs::path dir = fs::path(QUARTZ_COMPILER_PATH).parent_path() / "native-cache-test";
    fs::remove_all(dir);
    std::vector<quartz::SourceInput> src{{"host.qzi", readFile("bench/plugin/host.qzi")},
                                         {"plugin.qz", readFile("bench/plugin/plugin.qz")}};
    std::string expected = runPlugin(build(src), TierMode::Interpreter);
    const char* names[] = {"init", "onBreak", "onTransfer", "onChat", "onTick", "onSieve"};
    auto round = [&](std::uint32_t want) {
        auto p = build(src);
        CHECK(runPlugin(p, TierMode::Native, dir.string()) == expected);
        for (const char* f : names) {
            CAPTURE(f);
            CHECK(p->tier->stats(p->findFunction(f)).cache == want);
        }
    };
    round(quartz::vm::kNativeCacheStored);
    round(quartz::vm::kNativeCacheHit);
    for (const auto& e : fs::directory_iterator(dir)) {
        std::fstream f(e.path(), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(-1, std::ios::end);
        f.put('\x5a');
    }
    round(quartz::vm::kNativeCacheStored);
    round(quartz::vm::kNativeCacheHit);
    fs::remove_all(dir);
}

TEST_CASE("results do not depend on the native tier being available") {
    auto p = withStd("unavailable.qz", "fn add(a: int, b: int) -> int {\n    return a * 3 + b\n}\n");
    std::uint32_t f = p->findFunction("add");
    std::string log;
    for (TierMode mode : {TierMode::Native, TierMode::Auto}) {
        quartz::vm::Limits limits;
        limits.tier = mode;
        quartz::vm::Sandbox sb(p, limits);
        sb.bind("print", logText, &log);
        sb.bind("int.ticks", hostTicks);
        REQUIRE(sb.start().has_value());
        Slot a[2];
        a[0].i = 5;
        a[1].i = 2;
        for (int i = 0; i < 3; ++i) {
            auto r = sb.call(f, a);
            REQUIRE(r.has_value());
            CHECK(r->i == 17);
        }
    }
    bool native = p->tier->entry(f) != nullptr;
    MESSAGE("native tier ", native ? "available" : "unavailable: " + quartz::vm::nativeCompilerError());
    if (forcedUnavailable()) {
        CHECK_FALSE(native);
        CHECK_FALSE(p->tier->error(f).empty());
        CHECK_FALSE(quartz::vm::nativeCompilerError().empty());
    }
}

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

int main(int argc, char** argv) {
    setEnv("QUARTZ_COMPILER_LIB", QUARTZ_COMPILER_PATH);
    setEnv("QUARTZ_TIER_THRESHOLD", "1");
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
