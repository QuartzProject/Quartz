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
impl int {
    @property
    fn ticks(self) -> Delay
}
)";

// `// file: name` lines split one test into several source files.
std::vector<quartz::SourceInput> withInterface(const fs::path& path, const std::string& text) {
    std::vector<quartz::SourceInput> inputs{{path.string(), ""}};
    std::istringstream in(text);
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

void checkRun(const std::shared_ptr<const quartz::vm::Program>& program, const std::string& text) {
    std::string printed;
    quartz::vm::Sandbox sb(program);
    sb.bind("print", capturePrint, &printed);
    sb.bind("int.ticks", hostTicks);
    REQUIRE(sb.start().has_value());
    auto t = sb.spawn(program->findFunction("main"), {});
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

quartz::CompileOutput compileProgram(const fs::path& path, const std::string& text) {
    std::vector<quartz::SourceInput> inputs = withInterface(path, text);
    inputs.push_back({"<std>.qzi", std::string(kStd)});
    return quartz::compile(inputs, {.emitBinary = true, .manifest = {}});
}

} // namespace

TEST_CASE("programs print what their expect lines say") {
    for (const fs::path& path : files("tests/programs")) {
        CAPTURE(path.string());
        std::string text = readFile(path);
        quartz::CompileOutput out = compileProgram(path, text);
        REQUIRE_MESSAGE(out.program, out.diagnostics);
        for (const std::string& want : directives(text, "warning")) {
            CHECK_MESSAGE(out.diagnostics.find(want) != std::string::npos, want, "\n", out.diagnostics);
        }
        checkRun(out.program, text);
    }
}

TEST_CASE("binary modules decode to the same bytes and run the same") {
    for (const fs::path& path : files("tests/programs")) {
        CAPTURE(path.string());
        std::string text = readFile(path);
        quartz::CompileOutput out = compileProgram(path, text);
        REQUIRE_MESSAGE(out.program, out.diagnostics);
        auto mod = quartz::ir::deserialize(out.binary);
        REQUIRE_MESSAGE(mod.has_value(), (mod ? "" : mod.error()));
        CHECK(quartz::ir::serialize(**mod) == out.binary);
        CHECK(quartz::ir::serialize(*out.program->module) == out.binary);
        auto loaded = quartz::load(out.binary);
        REQUIRE_MESSAGE(loaded.has_value(), (loaded ? "" : loaded.error()));
        checkRun(*loaded, text);
    }
    quartz::SourceInput versioned[] = {
        {"v.qz", "@version(3)\nstate Hub {\n    @renamed(\"count\")\n    total: int\n}\n"}};
    quartz::CompileOutput out = quartz::compile(versioned, {.emitBinary = true, .manifest = {}});
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    auto mod = quartz::ir::deserialize(out.binary);
    REQUIRE(mod.has_value());
    const quartz::ir::TypeInfo& hub = (*mod)->types.info((*mod)->states.at(0));
    CHECK(hub.version == 3);
    CHECK(hub.fields.at(0).renamedFrom == "count");
    CHECK(quartz::ir::serialize(**mod) == out.binary);
}

TEST_CASE("truncated, corrupt or untrusted binary modules are rejected without crashing") {
    std::string text = readFile("tests/programs/core.qz");
    quartz::CompileOutput out = compileProgram("tests/programs/core.qz", text);
    REQUIRE(out.program);
    quartz::ir::SecretSeed seed{};
    seed.fill(7);
    quartz::ir::PublicKey key = quartz::ir::publicKey(seed);
    std::vector<std::uint8_t> file = out.binary;
    REQUIRE(quartz::ir::sign(file, seed).has_value());
    quartz::ir::ReadOptions strict;
    strict.trustedKeys = std::span(&key, 1);
    strict.requireSignature = true;
    REQUIRE(quartz::load(file, strict).has_value());
    CHECK_FALSE(quartz::load(out.binary, strict).has_value());

    for (std::size_t n = 0; n < file.size(); ++n) {
        CHECK_FALSE(quartz::load(std::span(file).first(n)).has_value());
    }
    for (std::size_t i = 0; i < file.size(); ++i) {
        std::vector<std::uint8_t> bad = file;
        bad[i] ^= 0x20;
        CHECK_FALSE(quartz::load(bad, strict).has_value());
    }
    std::vector<std::uint8_t> payload = quartz::ir::encodePayload(*out.program->module);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        for (int flip : {0x01, 0x80, 0xFF}) {
            std::vector<std::uint8_t> bad = payload;
            bad[i] = static_cast<std::uint8_t>(bad[i] ^ flip);
            (void)quartz::load(quartz::ir::seal(bad));
        }
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

TEST_CASE("the validator rejects function values that do not match their types") {
    quartz::SourceInput src[] = {{"v.qz", R"(state S {
    n: int = 0
}
fn inc(x: int) -> int {
    return x + 1
}
fn run(k: int) -> int {
    let f = (x: int) => x + k
    let g = inc
    return f(1) + g(2)
}
)"}};
    quartz::CompileOutput out = quartz::compile(src, {.emitBinary = true, .manifest = {}});
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    auto reject = [&](auto mutate, std::string_view why) {
        auto mod = quartz::ir::deserialize(out.binary);
        REQUIRE(mod.has_value());
        quartz::ir::Module& m = **mod;
        quartz::ir::Function& run = m.functions[out.program->findFunction("run")];
        std::vector<quartz::ir::Inst*> closures;
        quartz::ir::Inst* call = nullptr;
        for (quartz::ir::Inst& in : run.code) {
            if (in.op == quartz::ir::Op::Closure) {
                closures.push_back(&in);
            } else if (in.op == quartz::ir::Op::CallValue && call == nullptr) {
                call = &in;
            }
        }
        REQUIRE(closures.size() == 2);
        REQUIRE(call != nullptr);
        mutate(m, run, *closures[0], *closures[1], *call);
        auto loaded = quartz::load(quartz::ir::serialize(m));
        REQUIRE_FALSE(loaded.has_value());
        CHECK_MESSAGE(loaded.error().find(why) != std::string::npos, loaded.error());
    };
    using quartz::ir::Function;
    using quartz::ir::Inst;
    using quartz::ir::Module;
    reject([](Module&, Function&, Inst& lambda, Inst& named, Inst&) { lambda.imm = named.imm; },
           "closure type does not match its function");
    reject([](Module& m, Function&, Inst& lambda, Inst&, Inst&) { m.functions[lambda.imm].regs[1] = quartz::ir::kStr; },
           "capture 0 has the wrong type");
    reject([](Module& m, Function&, Inst& lambda, Inst&, Inst&) { m.functions[lambda.imm].isSuspend = true; },
           "a suspend function cannot be a value");
    reject([](Module&, Function&, Inst&, Inst&, Inst& call) { call.a = 0; },
           "call.value of a value that is not a function");
    reject([](Module&, Function& run, Inst&, Inst&, Inst& call) { run.regs[call.dst] = quartz::ir::kStr; },
           "call result type mismatch");
    reject([](Module&, Function& run, Inst& lambda, Inst&, Inst&) { run.regs[0] = run.regs[lambda.dst]; },
           "exported function takes or returns a function value");
    reject([](Module& m, Function& run, Inst& lambda, Inst&,
              Inst&) { m.imports.push_back({"h", {run.regs[lambda.dst]}, quartz::ir::kVoid}); },
           "import with invalid type");
    reject([](Module& m, Function& run, Inst& lambda, Inst&,
              Inst&) { m.types.mutableInfo(m.states[0]).fields[0].type = run.regs[lambda.dst]; },
           "state holds a function value");
    reject([](Module& m, Function&, Inst&, Inst&, Inst&) { m.types.fn({quartz::ir::kNever}, quartz::ir::kI64); },
           "malformed type");
}
