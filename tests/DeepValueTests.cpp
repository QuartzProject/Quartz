#define DOCTEST_CONFIG_IMPLEMENT
#include "quartz/Package.hpp"
#include "quartz/Quartz.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

using quartz::vm::Slot;
using quartz::vm::TierMode;

constexpr std::int64_t kDepth = 1'000'000;

constexpr const char* kStd = R"(@handle
@awaitable
struct Delay {}
impl int {
    @property
    fn ticks(self) -> Delay
}
)";

constexpr const char* kProvider = R"(package org.example.deep

@derive(Eq, Show)
enum Chain {
    Nil
    Cons(int, Chain)
}

struct Node {
    v: int
    kids: [Node]
}

state Deep {
    chain: Chain = Chain.Nil
    tree: Node = Node(0, [])
    byName: Map<str, Chain> = Map()
}

fn build(n: int) -> Chain {
    var c = Chain.Nil
    for i in 0..n {
        c = Chain.Cons(i, c)
    }
    return c
}

fn head(c: Chain) -> int {
    return match c {
        Nil => -1
        Cons(v, _) => v
    }
}

fn store(d: Deep, n: int) -> int {
    var kept = Chain.Nil
    scope {
        kept = build(n)
    }
    d.chain = kept
    d.byName["a"] = kept
    var t = Node(0, [])
    scope {
        var x = Node(0, [])
        for i in 0..n {
            x = Node(i, [x])
        }
        t = x
    }
    d.tree = t
    return head(d.chain) + d.tree.v
}

fn heads(d: Deep) -> int {
    return head(d.chain) + head(d.byName.get("a") ?? Chain.Nil) + d.tree.v
}

fn clear(d: Deep) -> int {
    d.chain = Chain.Nil
    d.byName.remove("a")
    d.tree = Node(0, [])
    return head(d.chain)
}

fn same(d: Deep) -> bool {
    return d.chain == (d.byName.get("a") ?? Chain.Nil)
}

fn text(d: Deep) -> str {
    return "{d.chain}"
}

suspend fn hold(d: Deep) -> int {
    let c = d.chain
    wait 1.ticks
    d.chain = Chain.Nil
    return head(c)
}

)";

constexpr const char* kClient = R"(import org.example.deep.*
fn fetch(n: int) -> int {
    return head(build(n))
}
)";

bool ticks(quartz::vm::HostCall& call, void*) {
    call.result.u = call.args[0].u;
    return true;
}

std::shared_ptr<const quartz::vm::Program> compile(std::vector<quartz::SourceInput> in, bool binary = false) {
    in.push_back({"<std>.qzi", kStd});
    quartz::CompileOptions opts;
    opts.emitBinary = binary;
    quartz::CompileOutput out = quartz::compile(in, opts);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    return out.program;
}

quartz::vm::Limits limitsFor(TierMode tier) {
    quartz::vm::Limits l;
    l.tier = tier;
    l.memoryBytes = std::size_t{1} << 30;
    l.fuelPerCall = 1'000'000'000;
    return l;
}

std::int64_t callInt(quartz::vm::Sandbox& sb, const char* fn, std::vector<Slot> args = {}) {
    auto r = sb.call(sb.program().findFunction(fn), args);
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    return r->i;
}

std::string trapOf(quartz::vm::Sandbox& sb, const char* fn) {
    auto r = sb.call(sb.program().findFunction(fn), {});
    return r ? "" : r.error().describe();
}

std::vector<TierMode> tiers() {
    std::vector<TierMode> out{TierMode::Interpreter};
    if (quartz::vm::nativeCompilerError().empty()) {
        out.push_back(TierMode::Native);
    }
    return out;
}

} // namespace

TEST_CASE("values a million levels deep are copied, stored, exported, imported, released, compared and shown") {
    auto p = compile({{"deep.qz", kProvider}});
    for (TierMode tier : tiers()) {
        CAPTURE(static_cast<int>(tier));
        quartz::vm::Sandbox sb(p, limitsFor(tier));
        sb.bind("int.ticks", ticks);
        REQUIRE(sb.start().has_value());
        Slot n{};
        n.i = kDepth;
        CHECK(callInt(sb, "store", {n}) == 2 * (kDepth - 1));
        CHECK(callInt(sb, "heads") == 3 * (kDepth - 1));
        auto saved = sb.exportState();
        REQUIRE_MESSAGE(saved.has_value(), (saved ? "" : saved.error().describe()));
        {
            quartz::vm::Sandbox copy(p, limitsFor(tier));
            copy.bind("int.ticks", ticks);
            REQUIRE(copy.start().has_value());
            auto back = copy.importState(*saved);
            REQUIRE_MESSAGE(back.has_value(), (back ? "" : back.error().describe()));
            CHECK(callInt(copy, "heads") == 3 * (kDepth - 1));
        }
        CHECK(trapOf(sb, "same").find("stack overflow") != std::string::npos);
        CHECK(trapOf(sb, "text").find("stack overflow") != std::string::npos);

        auto t = sb.spawn(p->findFunction("hold"), {});
        REQUIRE_MESSAGE(t.has_value(), (t ? "" : t.error().describe()));
        REQUIRE(t->kind == quartz::vm::TaskOutcome::Kind::Wait);
        t = sb.resume(t->task);
        REQUIRE_MESSAGE(t.has_value(), (t ? "" : t.error().describe()));
        CHECK(t->value.i == kDepth - 1);

        CHECK(callInt(sb, "clear") == -1);
        CHECK(callInt(sb, "store", {n}) == 2 * (kDepth - 1));
    }
}

TEST_CASE("a value a million levels deep crosses between linked sandboxes") {
    auto manifest =
        quartz::parseManifest("name = \"org.example.deep\"\nversion = \"1.0.0\"\nexports = [\"build\", \"head\"]\n");
    REQUIRE(manifest.has_value());
    auto provider = compile({{"deep.qz", kProvider}});
    auto iface = quartz::interfaceOf(*provider, *manifest);
    REQUIRE_MESSAGE(iface.has_value(), (iface ? "" : iface.error()));
    auto client = compile({{"deep.qzi", *iface}, {"client.qz", kClient}});
    for (TierMode tier : tiers()) {
        CAPTURE(static_cast<int>(tier));
        quartz::vm::Sandbox prov(provider, limitsFor(tier));
        quartz::vm::Sandbox user(client, limitsFor(tier));
        prov.bind("int.ticks", ticks);
        REQUIRE(user.link("org.example.deep", prov).has_value());
        REQUIRE(prov.start().has_value());
        REQUIRE(user.start().has_value());
        Slot n{};
        n.i = kDepth;
        CHECK(callInt(user, "fetch", {n}) == kDepth - 1);
    }
}

TEST_CASE("a recursive payload is saved as the value it holds, and documents that saved its box still load") {
    auto p = compile({{"chain.qz", R"(enum Chain {
    Nil
    Cons(int, Chain)
}
state Saved {
    chain: Chain = Chain.Nil
}
fn fill(s: Saved) {
    s.chain = Chain.Cons(1, Chain.Cons(2, Chain.Nil))
}
fn sum(s: Saved) -> int {
    var total = 0
    var c = s.chain
    while c is Cons(v, rest) {
        total = total * 10 + v
        c = rest
    }
    return total
}
)"}});
    quartz::vm::Sandbox sb(p);
    REQUIRE(sb.start().has_value());
    REQUIRE(sb.call(p->findFunction("fill"), {}).has_value());
    auto saved = sb.exportState();
    REQUIRE(saved.has_value());
    CHECK(*saved == R"({"format":2,"states":{"Saved":{"version":0,"fields":{"chain":{"variant":"Cons","values":[1,)"
                    R"({"variant":"Cons","values":[2,{"variant":"Nil","values":[]}]}]}}}},"handles":{}})");
    quartz::vm::Sandbox legacy(p);
    REQUIRE(legacy.start().has_value());
    auto r = legacy.importState(R"({"states":{"Saved":{"version":0,"fields":{"chain":{"variant":"Cons","values":[3,)"
                                R"([{"variant":"Nil","values":[]}]]}}}},"handles":{}})");
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    CHECK(callInt(legacy, "sum") == 3);
    REQUIRE(legacy.importState(*saved).has_value());
    CHECK(callInt(legacy, "sum") == 12);
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
#ifdef QUARTZ_COMPILER_PATH
    setEnv("QUARTZ_COMPILER_LIB", QUARTZ_COMPILER_PATH);
#else
    (void)&setEnv;
#endif
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
