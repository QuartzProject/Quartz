#include "quartz/Package.hpp"
#include "quartz/Quartz.hpp"

#include <doctest/doctest.h>
#include <array>
#include <cstring>
#include <string>

namespace {

std::shared_ptr<const quartz::vm::Program> build(const std::string& text) {
    quartz::SourceInput in[] = {{"plugin.qz", text}};
    quartz::CompileOutput out = quartz::compile(in);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    return out.program;
}

std::int64_t callInt(quartz::vm::Sandbox& sb, const char* fn) {
    auto r = sb.call(sb.program().findFunction(fn), {});
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    return r->i;
}

constexpr const char* kV1 = R"(
@version(1)
state Hub {
    visits: Map<str, i64>
    count: i64
    old: str
}
fn fill(hub: Hub) {
    hub.visits["Steve"] = 3
    hub.count = 7
    hub.old = "bye"
}
)";

} // namespace

TEST_CASE("reload carries state over by field name, honours @renamed and refuses unsafe changes") {
    auto v1 = build(kV1);
    quartz::vm::Sandbox sb(v1);
    REQUIRE(sb.start().has_value());
    REQUIRE(sb.call(v1->findFunction("fill"), {}).has_value());

    auto v2 = build(R"(
@version(2)
state Hub {
    visits: Map<str, i64>
    @renamed("count")
    total: i64
    added: i64 = 40
}
fn check(hub: Hub) -> i64 {
    return (hub.visits.get("Steve") ?? 0) * 1000 + hub.total * 100 + hub.added
}
)");
    auto next = sb.reload(v2);
    REQUIRE_MESSAGE(next.has_value(), (next ? "" : next.error().describe()));
    CHECK(callInt(**next, "check") == 3740);

    auto incompatible = sb.reload(build(R"(
@version(2)
state Hub {
    visits: Map<str, str>
}
)"));
    REQUIRE_FALSE(incompatible.has_value());
    CHECK(incompatible.error().message.find("migrateHub") != std::string::npos);

    auto migrated = sb.reload(build(R"(
import std.data.*
@version(2)
state Hub {
    visits: Map<str, str>
    seen: i64
}
fn migrateHub(old: Value, hub: Hub) {
    hub.visits["Steve"] = "{old.get("visits")?.get("Steve")?.asInt() ?? -1}"
    hub.seen = old.get("count")?.asInt() ?? -1
}
fn check(hub: Hub) -> i64 {
    return hub.seen * 10 + (hub.visits["Steve"].len())
}
)"));
    REQUIRE_MESSAGE(migrated.has_value(), (migrated ? "" : migrated.error().describe()));
    CHECK(callInt(**migrated, "check") == 71);

    auto older = (*next)->reload(build(kV1));
    REQUIRE_FALSE(older.has_value());
    CHECK(older.error().message.find("newer") != std::string::npos);
}

TEST_CASE("per-handle states are separate per handle and survive a save and load") {
    quartz::SourceInput in[] = {{"host.qzi", "@handle\nstruct Player {\n    id: i64\n}\n"},
                                {"plugin.qz", R"(
@persist(per = Player)
state Stats {
    joins: i64
    title: str = "new"
}
fn join(p: Player, s: Stats) -> i64 {
    s.joins += 1
    s.title = "veteran"
    return s.joins
}
)"}};
    quartz::CompileOutput out = quartz::compile(in);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    quartz::vm::Sandbox sb(out.program);
    sb.bind("Player.id", [](quartz::vm::HostCall& c, void*) { c.result.u = c.args[0].u; return true; });
    REQUIRE(sb.start().has_value());
    auto join = [&](std::uint64_t handle) {
        quartz::vm::Slot a{};
        a.u = handle;
        auto r = sb.call(out.program->findFunction("join"), std::span<const quartz::vm::Slot>(&a, 1));
        REQUIRE(r.has_value());
        return r->i;
    };
    CHECK(join(7) == 1);
    CHECK(join(7) == 2);
    CHECK(join(9) == 1);
    auto saved = sb.exportHandle(7);
    REQUIRE(saved.has_value());
    sb.dropHandle(7);
    CHECK(join(7) == 1);
    sb.dropHandle(7);
    REQUIRE(sb.importHandle(42, *saved).has_value());
    CHECK(join(42) == 3);
}

TEST_CASE("packages call each other through generated interfaces and load in dependency order") {
    quartz::SourceInput eco[] = {{"eco.qz", R"(package economy
state Bank {
    money: Map<str, i64>
}
fn balance(bank: Bank, who: str) -> i64 {
    return bank.money.get(who) ?? 100
}
fn pay(bank: Bank, who: str, amount: i64) -> str {
    bank.money[who] = balance(bank, who) + amount
    return "{who}={bank.money[who]}"
}
)"}};
    auto manifest = quartz::parseManifest("name = \"economy\"\nversion = \"1.4.2\"\nexports = [\"balance\", \"pay\"]\n");
    REQUIRE(manifest.has_value());
    quartz::CompileOutput ecoOut = quartz::compile(eco);
    REQUIRE_MESSAGE(ecoOut.program, ecoOut.diagnostics);
    auto iface = quartz::interfaceOf(*ecoOut.program, *manifest);
    REQUIRE_MESSAGE(iface.has_value(), (iface ? "" : iface.error()));
    quartz::SourceInput shop[] = {{"economy.qzi", *iface}, {"shop.qz", R"(import economy.*
fn buy(who: str) -> str {
    let receipt = pay(who, 0 - 30)
    return "{receipt} left {balance(who)}"
}
)"}};
    quartz::CompileOutput shopOut = quartz::compile(shop);
    REQUIRE_MESSAGE(shopOut.program, shopOut.diagnostics);
    quartz::vm::Sandbox provider(ecoOut.program);
    quartz::vm::Sandbox client(shopOut.program);
    REQUIRE(client.link("economy", provider).has_value());
    REQUIRE(provider.start().has_value());
    REQUIRE(client.start().has_value());
    quartz::vm::Slot who{};
    who.p = client.newStr("ana");
    auto r = client.call(shopOut.program->findFunction("buy"), std::span<const quartz::vm::Slot>(&who, 1));
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    CHECK(static_cast<const quartz::vm::Str*>(r->p)->view() == "ana=70 left 70");

    auto cycle = quartz::loadOrder({*quartz::parseManifest("name = \"a\"\nversion = \"1.0.0\"\n[dependencies]\nb = \"^1\"\n"),
                                    *quartz::parseManifest("name = \"b\"\nversion = \"1.2.0\"\n[dependencies]\na = \"*\"\n")});
    REQUIRE_FALSE(cycle.has_value());
    CHECK(cycle.error().find("cycle") != std::string::npos);
    auto order = quartz::loadOrder({*quartz::parseManifest("name = \"shop\"\nversion = \"0.1.0\"\n[dependencies]\neconomy = \">=1.2, <2\"\n"),
                                    *manifest});
    REQUIRE(order.has_value());
    CHECK(*order == std::vector<std::size_t>{1, 0});
    auto tooNew = quartz::loadOrder({*quartz::parseManifest("name = \"shop\"\nversion = \"0.1.0\"\n[dependencies]\neconomy = \"^2\"\n"),
                                     *manifest});
    CHECK_FALSE(tooNew.has_value());
}

TEST_CASE("a package loads only when signed by the key trusted for its name") {
    quartz::SourceInput src[] = {{"eco.qz", "package economy\nfn one() -> i64 {\n    return 1\n}\n"}};
    quartz::CompileOptions opts;
    opts.emitBinary = true;
    opts.manifest = "name = \"economy\"\nversion = \"1.0.0\"\n";
    quartz::CompileOutput out = quartz::compile(src, opts);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    std::array<std::uint8_t, 32> publisher{};
    std::array<std::uint8_t, 32> stranger{};
    publisher.fill(7);
    stranger.fill(9);
    std::vector<std::uint8_t> signedBytes = out.binary;
    REQUIRE(quartz::ir::sign(signedBytes, publisher).has_value());
    quartz::TrustPolicy policy;
    policy.keys.emplace_back("economy", quartz::ir::publicKey(publisher));
    auto ok = quartz::loadPackage(signedBytes, policy);
    REQUIRE_MESSAGE(ok.has_value(), (ok ? "" : ok.error()));
    CHECK(ok->manifest.name == "economy");
    CHECK_FALSE(quartz::loadPackage(out.binary, policy).has_value());
    quartz::TrustPolicy wrong;
    wrong.keys.emplace_back("economy", quartz::ir::publicKey(stranger));
    CHECK_FALSE(quartz::loadPackage(signedBytes, wrong).has_value());
    CHECK_FALSE(quartz::loadPackage(signedBytes, quartz::TrustPolicy{}).has_value());
}

TEST_CASE("repr(C) structs reach the host with the C layout and come back") {
    struct CVec {
        float x;
        std::int16_t y;
        double z;
    };
    static_assert(sizeof(CVec) == 16);
    quartz::SourceInput in[] = {{"host.qzi", "@repr(C)\nstruct Vec {\n    x: f32\n    y: i16\n    z: f64\n}\n"
                                             "fn scale(v: Vec, k: i32) -> Vec\n"},
                                {"plugin.qz", R"(
fn run() -> f64 {
    let v = scale(Vec(1.5, -3, 0.25), 4)
    return (v.x as f64) * 100.0 + (v.y as f64) * 10.0 + v.z
}
)"}};
    quartz::CompileOutput out = quartz::compile(in);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    quartz::vm::Sandbox sb(out.program);
    sb.bind("scale", [](quartz::vm::HostCall& c, void*) {
        CVec v{};
        std::memcpy(&v, c.args[0].p, sizeof v);
        auto k = static_cast<std::int32_t>(c.args[1].i);
        CVec r{v.x * static_cast<float>(k), static_cast<std::int16_t>(v.y * k), v.z * k};
        std::memcpy(c.result.p, &r, sizeof r);
        return true;
    });
    REQUIRE(sb.start().has_value());
    auto r = sb.call(out.program->findFunction("run"), {});
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    CHECK(r->f == 600.0 - 120.0 + 1.0);
}

TEST_CASE("the host passes a struct argument spread over its slots, to a call and to a task") {
    quartz::SourceInput in[] = {{"host.qzi", "fn note(text: str)\n"},
                                {"plugin.qz", R"(
struct Event {
    level: i64
    name: str
    flag: bool
}
fn total(e: Event, extra: i64) -> i64 {
    var n = e.level + extra
    if e.flag {
        n += e.name.len()
    }
    return n
}
suspend fn task(e: Event) -> i64 {
    return e.level * 2 + e.name.len()
}
)"}};
    quartz::CompileOutput out = quartz::compile(in);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    quartz::vm::Sandbox sb(out.program);
    sb.bind("note", [](quartz::vm::HostCall&, void*) { return true; });
    REQUIRE(sb.start().has_value());
    quartz::vm::Slot args[4]{};
    args[0].i = 5;
    args[1].p = sb.newStr("Steve");
    args[2].i = 1;
    args[3].i = 10;
    auto r = sb.call(out.program->findFunction("total"), args);
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    CHECK(r->i == 20);
    auto t = sb.spawn(out.program->findFunction("task"), std::span<const quartz::vm::Slot>(args, 3));
    REQUIRE(t.has_value());
    CHECK(t->value.i == 15);
    CHECK_FALSE(sb.call(out.program->findFunction("total"), std::span<const quartz::vm::Slot>(args, 3)).has_value());
}
