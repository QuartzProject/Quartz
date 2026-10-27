#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "ir/Validator.hpp"
#include "quartz/Quartz.hpp"
#include "std/Std.hpp"

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
        for (const std::string& want : traps) {
            CHECK_MESSAGE(r.error().describe().find(want) != std::string::npos, want, "\n", r.error().describe());
        }
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
        std::vector<std::string> wanted = directives(text, "warning");
        for (const std::string& want : wanted) {
            CHECK_MESSAGE(out.diagnostics.find(want) != std::string::npos, want, "\n", out.diagnostics);
        }
        CHECK_MESSAGE(out.warnings == wanted.size(), out.diagnostics);
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

TEST_CASE("builtin intrinsics given another builtin's operands are rejected") {
    quartz::SourceInput in[] = {{"v.qz", "struct P {\n    x: int\n}\nfn f(xs: [int], ps: [P]) -> bool {\n"
                                         "    ps.reverse()\n    return xs.contains(1)\n}\n"}};
    quartz::CompileOutput out = quartz::compile(in, {.emitBinary = true, .manifest = {}});
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    auto mod = quartz::ir::deserialize(out.binary);
    REQUIRE(mod.has_value());
    using quartz::ir::Intrinsic;
    int checked = 0;
    for (quartz::ir::Function& fn : (*mod)->functions) {
        for (quartz::ir::Inst& inst : fn.code) {
            if (inst.op != quartz::ir::Op::Intrinsic || inst.imm < static_cast<std::uint32_t>(Intrinsic::StrToInt)) {
                continue;
            }
            std::uint32_t own = inst.imm;
            for (auto op = static_cast<std::uint32_t>(Intrinsic::StrToInt);
                 op < static_cast<std::uint32_t>(Intrinsic::Count); ++op) {
                inst.imm = op;
                CHECK_MESSAGE(quartz::ir::validate(**mod).has_value() == (op == own), op);
                ++checked;
            }
            inst.imm = own;
        }
    }
    CHECK(checked == 2 * static_cast<int>(static_cast<std::uint32_t>(Intrinsic::Count) -
                                          static_cast<std::uint32_t>(Intrinsic::StrToInt)));
}

TEST_CASE("the standard library has no lint warnings") {
    std::string imports = "package test.lint\n";
    for (const auto& [name, text] : quartz::stdlib::sources()) {
        imports += "import " + std::string(quartz::stdlib::packageOf(text)) + ".*\n";
    }
    quartz::SourceInput in[] = {{"lint.qz", imports, true}, {"<std>.qzi", std::string(kStd)}};
    quartz::CompileOutput out = quartz::compile(in, {.manifest = {}, .lintLibrary = true});
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    CHECK_MESSAGE(out.warnings == 0, out.diagnostics);
}

TEST_CASE("invalid programs report the expected diagnostics") {
    for (const fs::path& path : files("tests/diagnostics")) {
        CAPTURE(path.string());
        std::string text = readFile(path);
        quartz::CompileOutput out = quartz::compile(withInterface(path, text));
        std::vector<std::string> errors = directives(text, "error");
        std::vector<std::string> warnings = directives(text, "warning");
        CHECK(!out.program == !errors.empty());
        for (const std::string& want : errors) {
            CHECK_MESSAGE(out.diagnostics.find(want) != std::string::npos, want, "\n", out.diagnostics);
        }
        for (const std::string& want : warnings) {
            CHECK_MESSAGE(out.diagnostics.find("warning: " + want) != std::string::npos, want, "\n", out.diagnostics);
        }
        if (errors.empty()) {
            CHECK_MESSAGE(out.warnings == warnings.size(), out.diagnostics);
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

TEST_CASE("the validator rejects fixed-size array accesses that do not match the array") {
    quartz::SourceInput src[] = {{"a.qz", R"(fn run(i: int, s: str) -> str {
    var a: [str; 2] = ["x", "y"]
    a[i] = s
    let t = (i, 2)
    return a[1 - i] + "{t.1}"
}
)"}};
    quartz::CompileOutput out = quartz::compile(src, {.emitBinary = true, .manifest = {}});
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    using quartz::ir::Function;
    using quartz::ir::Inst;
    using quartz::ir::Module;
    auto reject = [&](auto mutate, std::string_view why) {
        auto mod = quartz::ir::deserialize(out.binary);
        REQUIRE(mod.has_value());
        Module& m = **mod;
        Function& run = m.functions[out.program->findFunction("run")];
        Inst* get = nullptr;
        Inst* set = nullptr;
        Inst* make = nullptr;
        for (Inst& in : run.code) {
            get = in.op == quartz::ir::Op::ArrayGet ? &in : get;
            set = in.op == quartz::ir::Op::ArraySet ? &in : set;
            bool tuple = in.op == quartz::ir::Op::Make && !m.types.isArray(run.regs[in.dst]);
            make = tuple ? &in : make;
        }
        REQUIRE(get != nullptr);
        REQUIRE(set != nullptr);
        REQUIRE(make != nullptr);
        mutate(m, run, *get, *set, *make);
        auto v = quartz::ir::validate(m);
        REQUIRE_FALSE(v.has_value());
        CHECK_MESSAGE(v.error().describe(m).find(why) != std::string::npos, v.error().describe(m));
    };
    reject([](Module&, Function&, Inst& get, Inst&, Inst& tuple) { get.a = tuple.dst; },
           "array access on a value that is not a fixed-size array");
    reject([](Module&, Function&, Inst& get, Inst& set, Inst&) { get.b = set.b; }, "array index must be an int");
    reject([](Module&, Function& run, Inst& get, Inst&, Inst&) { run.regs[get.dst] = quartz::ir::kI64; },
           "array element type mismatch");
    reject([](Module&, Function& run, Inst&, Inst& set, Inst&) { set.b = run.args[set.argBegin]; },
           "array element type mismatch");
    reject([](Module&, Function&, Inst&, Inst& set, Inst&) { set.argCount = 0; }, "array index must be an int");
    reject([](Module& m, Function& run, Inst& get, Inst&,
              Inst&) { m.types.mutableInfo(run.regs[get.a]).elems[1] = quartz::ir::kI64; },
           "malformed type");
    reject([](Module& m, Function&, Inst&, Inst&,
              Inst&) { m.types.array(quartz::ir::kI64, quartz::ir::kMaxArrayLength + 1); },
           "malformed type");
}

TEST_CASE("a recursive enum is valid only with its recursive payload boxed in a list") {
    quartz::SourceInput src[] = {{"r.qz", R"(enum Expr {
    Num(int)
    Neg(Expr)
}
fn run(n: int) -> int {
    return match Expr.Neg(Expr.Num(n)) {
        Neg(Num(v)) => v
        _ => 0
    }
}
)"}};
    quartz::CompileOutput out = quartz::compile(src, {.emitBinary = true, .manifest = {}});
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    auto mod = quartz::ir::deserialize(out.binary);
    REQUIRE(mod.has_value());
    quartz::ir::Module& m = **mod;
    quartz::ir::TypeId expr = quartz::ir::kError;
    for (quartz::ir::TypeId t = quartz::ir::kFirstUserType; t < m.types.size(); ++t) {
        expr = m.types.info(t).name == "Expr" ? t : expr;
    }
    REQUIRE(expr != quartz::ir::kError);
    REQUIRE(m.types.kind(m.types.info(expr).variants[1].payload[0]) == quartz::ir::TypeKind::List);
    REQUIRE(m.types.info(expr).variants[1].isBoxed(0));
    m.types.mutableInfo(expr).variants[0].boxed = {true};
    auto boxedInt = quartz::ir::validate(m);
    REQUIRE_FALSE(boxedInt.has_value());
    CHECK(boxedInt.error().describe(m).find("boxes a payload that is not a list") != std::string::npos);
    m.types.mutableInfo(expr).variants[0].boxed.clear();
    m.types.mutableInfo(expr).variants[1].boxed.clear();
    m.types.mutableInfo(expr).variants[1].payload[0] = m.types.nullable(expr);
    auto v = quartz::ir::validate(m);
    REQUIRE_FALSE(v.has_value());
    CHECK_MESSAGE(v.error().describe(m).find("contains itself by value") != std::string::npos, v.error().describe(m));
}

TEST_CASE("the validator rejects map iteration, text decoding and formatting with mismatched types") {
    quartz::SourceInput src[] = {{"i.qz", R"(fn run(m: Map<str, int>, t: str) -> int {
    var n = 0
    for k, v in m {
        n += v + k.len()
    }
    for c in t {
        n += c as int
    }
    return n + "{t:>9}".len()
}
)"}};
    quartz::CompileOutput out = quartz::compile(src, {.emitBinary = true, .manifest = {}});
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    using quartz::ir::Intrinsic;
    auto reject = [&](Intrinsic which, auto mutate) {
        auto mod = quartz::ir::deserialize(out.binary);
        REQUIRE(mod.has_value());
        quartz::ir::Function& run = (*mod)->functions[out.program->findFunction("run")];
        quartz::ir::Inst* found = nullptr;
        for (quartz::ir::Inst& in : run.code) {
            if (in.op == quartz::ir::Op::Intrinsic && in.imm == static_cast<std::uint32_t>(which) && found == nullptr) {
                found = &in;
            }
        }
        REQUIRE(found != nullptr);
        mutate(run, *found);
        auto loaded = quartz::load(quartz::ir::serialize(**mod));
        REQUIRE_FALSE(loaded.has_value());
        CHECK_MESSAGE(loaded.error().find("bad operand types for intrinsic") != std::string::npos, loaded.error());
    };
    reject(Intrinsic::MapNext, [](quartz::ir::Function& f, quartz::ir::Inst& in) {
        std::swap(f.args[in.argBegin], f.args[in.argBegin + 1]);
    });
    reject(Intrinsic::MapKeyAt, [](quartz::ir::Function&, quartz::ir::Inst& in) {
        in.imm = static_cast<std::uint32_t>(Intrinsic::MapValueAt);
    });
    reject(Intrinsic::MapValueAt, [](quartz::ir::Function&, quartz::ir::Inst& in) {
        in.imm = static_cast<std::uint32_t>(Intrinsic::MapKeyAt);
    });
    reject(Intrinsic::StrDecode,
           [](quartz::ir::Function& f, quartz::ir::Inst& in) { in.dst = f.args[in.argBegin + 1]; });
    reject(Intrinsic::StrFormat, [](quartz::ir::Function& f, quartz::ir::Inst& in) {
        std::swap(f.args[in.argBegin], f.args[in.argBegin + 1]);
    });
}

TEST_CASE("the validator rejects a wait producing a state or a function value") {
    quartz::SourceInput src[] = {{"host.qzi", "struct R {\n    n: int\n}\n@handle\n@awaitable(R)\nstruct Ask {}\n"
                                              "fn ask() -> Ask\n"},
                                 {"w.qz", "state S {\n    n: int = 0\n}\nsuspend fn run(s: S) -> int {\n"
                                          "    let r = wait ask()\n    return r.n + s.n\n}\n"}};
    quartz::CompileOutput out = quartz::compile(src, {.emitBinary = true, .manifest = {}});
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    auto reject = [&](auto produced) {
        auto mod = quartz::ir::deserialize(out.binary);
        REQUIRE(mod.has_value());
        quartz::ir::Function& run = (*mod)->functions[out.program->findFunction("run")];
        quartz::ir::Inst* wait = nullptr;
        for (quartz::ir::Inst& in : run.code) {
            wait = in.op == quartz::ir::Op::Wait && wait == nullptr ? &in : wait;
        }
        REQUIRE(wait != nullptr);
        run.regs[wait->dst] = produced(**mod);
        auto loaded = quartz::load(quartz::ir::serialize(**mod));
        REQUIRE_FALSE(loaded.has_value());
        CHECK_MESSAGE(loaded.error().find("wait cannot produce") != std::string::npos, loaded.error());
    };
    using quartz::ir::kI64;
    reject([](quartz::ir::Module& m) { return m.states[0]; });
    reject([](quartz::ir::Module& m) { return m.types.fn({kI64}, kI64); });
    reject([](quartz::ir::Module& m) { return m.types.list(m.types.fn({kI64}, kI64)); });
}

TEST_CASE("the validator rejects local names outside their function") {
    quartz::SourceInput src[] = {{"n.qz", "fn f(a: int) -> int {\n    let b = a + 1\n    return b\n}\n"}};
    quartz::CompileOutput out = quartz::compile(src, {.emitBinary = true, .manifest = {}});
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    auto reject = [&](auto mutate) {
        auto mod = quartz::ir::deserialize(out.binary);
        REQUIRE(mod.has_value());
        quartz::ir::Function& f = (*mod)->functions[out.program->findFunction("f")];
        REQUIRE(f.locals.size() == 2);
        mutate(f);
        auto v = quartz::ir::validate(**mod);
        REQUIRE_FALSE(v.has_value());
        CHECK(v.error().message == "local name outside its function");
    };
    reject([](quartz::ir::Function& f) { f.locals[1].reg = static_cast<quartz::ir::Reg>(f.regs.size()); });
    reject([](quartz::ir::Function& f) { f.locals[1].to = static_cast<std::uint32_t>(f.code.size()) + 1; });
    reject([](quartz::ir::Function& f) { f.locals[1].from = f.locals[1].to + 1; });
}
