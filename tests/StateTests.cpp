#include "quartz/Bind.hpp"
#include "quartz/Package.hpp"
#include "quartz/Quartz.hpp"
#include "vm/Layout.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

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
    visits: Map<str, int>
    count: int
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
    visits: Map<str, int>
    @renamed("count")
    total: int
    added: int = 40
}
fn check(hub: Hub) -> int {
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
import org.quartzlang.data.*
@version(2)
state Hub {
    visits: Map<str, str>
    seen: int
}
fn migrateHub(old: Value, hub: Hub) {
    hub.visits["Steve"] = "{old.get("visits")?.get("Steve")?.asInt() ?? -1}"
    hub.seen = old.get("count")?.asInt() ?? -1
}
fn check(hub: Hub) -> int {
    return hub.seen * 10 + (hub.visits["Steve"].len())
}
)"));
    REQUIRE_MESSAGE(migrated.has_value(), (migrated ? "" : migrated.error().describe()));
    CHECK(callInt(**migrated, "check") == 71);

    auto older = (*next)->reload(build(kV1));
    REQUIRE_FALSE(older.has_value());
    CHECK(older.error().message.find("newer") != std::string::npos);
}

TEST_CASE("reload keeps maps in insertion order, struct keys included") {
    constexpr const char* kGrid = R"(
@derive(Eq, Hash)
struct Pos {
    x: int
    y: int
}
state Grid {
    cells: Map<Pos, str>
    names: Map<str, int>
}
fn fill(g: Grid) {
    g.cells[Pos(2, 0)] = "b"
    g.cells[Pos(1, 0)] = "a"
    g.names["z"] = 1
    g.names["a"] = 2
}
fn order(g: Grid) -> str {
    var out = ""
    for p, s in g.cells {
        out += "{p.x}{s}"
    }
    for n in g.names {
        out += n
    }
    return out
}
)";
    auto v1 = build(kGrid);
    quartz::vm::Sandbox sb(v1);
    REQUIRE(sb.start().has_value());
    REQUIRE(sb.call(v1->findFunction("fill"), {}).has_value());
    auto next = sb.reload(build(kGrid));
    REQUIRE_MESSAGE(next.has_value(), (next ? "" : next.error().describe()));
    auto r = (*next)->call((*next)->program().findFunction("order"), {});
    REQUIRE(r.has_value());
    CHECK(static_cast<const quartz::vm::Str*>(r->p)->view() == "2b1aza");
}

TEST_CASE("per-handle states are separate per handle and survive a save and load") {
    quartz::SourceInput in[] = {{"host.qzi", "@handle\nstruct Player {\n    id: int\n}\n"},
                                {"plugin.qz", R"(
@persist(per = Player)
state Stats {
    joins: int
    title: str = "new"
}
fn join(p: Player, s: Stats) -> int {
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
    quartz::SourceInput eco[] = {{"eco.qz", R"(package org.example.economy
state Bank {
    money: Map<str, int>
}
fn balance(bank: Bank, who: str) -> int {
    return bank.money.get(who) ?? 100
}
fn pay(bank: Bank, who: str, amount: int) -> str {
    bank.money[who] = balance(bank, who) + amount
    return "{who}={bank.money[who]}"
}
)"}};
    auto manifest = quartz::parseManifest("name = \"org.example.economy\"\nversion = \"1.4.2\"\nexports = [\"balance\", \"pay\"]\n");
    REQUIRE(manifest.has_value());
    quartz::CompileOutput ecoOut = quartz::compile(eco);
    REQUIRE_MESSAGE(ecoOut.program, ecoOut.diagnostics);
    auto iface = quartz::interfaceOf(*ecoOut.program, *manifest);
    REQUIRE_MESSAGE(iface.has_value(), (iface ? "" : iface.error()));
    quartz::SourceInput shop[] = {{"economy.qzi", *iface}, {"shop.qz", R"(import org.example.economy.*
fn buy(who: str) -> str {
    let receipt = pay(who, 0 - 30)
    return "{receipt} left {balance(who)}"
}
)"}};
    quartz::CompileOutput shopOut = quartz::compile(shop);
    REQUIRE_MESSAGE(shopOut.program, shopOut.diagnostics);
    quartz::vm::Sandbox provider(ecoOut.program);
    quartz::vm::Sandbox client(shopOut.program);
    REQUIRE(client.link("org.example.economy", provider).has_value());
    REQUIRE(provider.start().has_value());
    REQUIRE(client.start().has_value());
    quartz::vm::Slot who{};
    who.p = client.newStr("ana");
    auto r = client.call(shopOut.program->findFunction("buy"), std::span<const quartz::vm::Slot>(&who, 1));
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    CHECK(static_cast<const quartz::vm::Str*>(r->p)->view() == "ana=70 left 70");

    auto cycle = quartz::loadOrder({*quartz::parseManifest("name = \"org.example.a\"\nversion = \"1.0.0\"\n[dependencies]\n\"org.example.b\" = \"^1\"\n"),
                                    *quartz::parseManifest("name = \"org.example.b\"\nversion = \"1.2.0\"\n[dependencies]\n\"org.example.a\" = \"*\"\n")});
    REQUIRE_FALSE(cycle.has_value());
    CHECK(cycle.error().find("cycle") != std::string::npos);
    auto order = quartz::loadOrder({*quartz::parseManifest("name = \"org.example.shop\"\nversion = \"0.1.0\"\n[dependencies]\n\"org.example.economy\" = \">=1.2, <2\"\n"),
                                    *manifest});
    REQUIRE(order.has_value());
    CHECK(*order == std::vector<std::size_t>{1, 0});
    auto tooNew = quartz::loadOrder({*quartz::parseManifest("name = \"org.example.shop\"\nversion = \"0.1.0\"\n[dependencies]\n\"org.example.economy\" = \"^2\"\n"),
                                     *manifest});
    CHECK_FALSE(tooNew.has_value());
}

TEST_CASE("a package loads only when signed by the key trusted for its name") {
    quartz::SourceInput src[] = {{"eco.qz", "package org.example.economy\nfn one() -> int {\n    return 1\n}\n"}};
    quartz::CompileOptions opts;
    opts.emitBinary = true;
    opts.manifest = "name = \"org.example.economy\"\nversion = \"1.0.0\"\n";
    quartz::CompileOutput out = quartz::compile(src, opts);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    std::array<std::uint8_t, 32> publisher{};
    std::array<std::uint8_t, 32> stranger{};
    publisher.fill(7);
    stranger.fill(9);
    std::vector<std::uint8_t> signedBytes = out.binary;
    REQUIRE(quartz::ir::sign(signedBytes, publisher).has_value());
    quartz::TrustPolicy policy;
    policy.keys.emplace_back("org.example.economy", quartz::ir::publicKey(publisher));
    auto ok = quartz::loadPackage(signedBytes, policy);
    REQUIRE_MESSAGE(ok.has_value(), (ok ? "" : ok.error()));
    CHECK(ok->manifest.name == "org.example.economy");
    CHECK_FALSE(quartz::loadPackage(out.binary, policy).has_value());
    quartz::TrustPolicy wrong;
    wrong.keys.emplace_back("org.example.economy", quartz::ir::publicKey(stranger));
    CHECK_FALSE(quartz::loadPackage(signedBytes, wrong).has_value());
    CHECK_FALSE(quartz::loadPackage(signedBytes, quartz::TrustPolicy{}).has_value());
}

TEST_CASE("repr(C) structs reach the host with the C layout and come back") {
    struct CVec {
        float x;
        std::int16_t y;
        double z;
        std::int32_t w[3];
    };

    static_assert(sizeof(CVec) == 32);
    quartz::SourceInput in[] = {{"host.qzi", "@repr(C)\nstruct Vec {\n    x: f32\n    y: i16\n    z: f64\n"
                                             "    w: [i32; 3]\n}\nfn scale(v: Vec, k: i32) -> Vec\n"},
                                {"plugin.qz", R"(
fn run() -> float {
    let v = scale(Vec(1.5, -3, 0.25, [1, 2, -3]), 4)
    return (v.x as float) * 100.0 + (v.y as float) * 10.0 + v.z + (v.w[0] + v.w[2]) as float * 1000.0
}
)"}};
    quartz::CompileOutput out = quartz::compile(in);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    quartz::vm::Sandbox sb(out.program);
    sb.bind("scale", [](quartz::vm::HostCall& c, void*) {
        CVec v{};
        std::memcpy(&v, c.args[0].p, sizeof v);
        auto k = static_cast<std::int32_t>(c.args[1].i);
        CVec r{v.x * static_cast<float>(k), static_cast<std::int16_t>(v.y * k), v.z * k, {v.w[0] * k, v.w[1], v.w[2]}};
        std::memcpy(c.result.p, &r, sizeof r);
        return true;
    });
    REQUIRE(sb.start().has_value());
    auto r = sb.call(out.program->findFunction("run"), {});
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    CHECK(r->f == 600.0 - 120.0 + 1.0 + (4.0 - 3.0) * 1000.0);
}

TEST_CASE("the host passes a struct argument spread over its slots, to a call and to a task") {
    quartz::SourceInput in[] = {{"host.qzi", "fn note(text: str)\n"},
                                {"plugin.qz", R"(
struct Event {
    level: int
    name: str
    flag: bool
}
fn total(e: Event, extra: int) -> int {
    var n = e.level + extra
    if e.flag {
        n += e.name.len()
    }
    return n
}
suspend fn task(e: Event) -> int {
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

TEST_CASE("sandboxes of one program run on separate threads at once") {
    auto program = build(R"(import org.quartzlang.data.*
state Tally {
    seen: Map<str, int>
}
fn work(t: Tally, n: int) -> int {
    var total = 0
    for i in 0..n {
        let key = "k{i % 7}"
        t.seen[key] = (t.seen.get(key) ?? 0) + i
        total = total + (parseJson("[{i}]").unwrap().item(0)?.asInt() ?? 0)
    }
    return total + t.seen.len()
}
)");
    std::vector<std::int64_t> results(8, 0);
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < results.size(); ++t) {
        threads.emplace_back([&, t] {
            quartz::SourceInput in[] = {{"again.qz", "fn twice(x: int) -> int {\n    return x * 2\n}\n"}};
            bool compiled = quartz::compile(in).program != nullptr;
            quartz::vm::Sandbox sb(program);
            if (!compiled || !sb.start()) {
                return;
            }
            quartz::vm::Slot n{};
            n.i = 300;
            for (int round = 0; round < 20; ++round) {
                auto r = sb.call(program->findFunction("work"), std::span<const quartz::vm::Slot>(&n, 1));
                results[t] = r ? r->i : -1;
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    for (std::int64_t r : results) {
        CHECK(r == 44850 + 7);
    }
}

namespace {

std::string shout(std::string_view text, std::int32_t times) {
    std::string out;
    for (std::int32_t i = 0; i < times; ++i) {
        out += text;
    }
    return out + "!";
}

bool cheap(quartz::vm::HostCall& call, double x) { return call.charge(1) && x > 0.5; }

} // namespace

TEST_CASE("typed host bindings convert arguments and reject a signature that differs from the interface") {
    quartz::SourceInput in[] = {{"host.qzi", "fn shout(text: str, times: i32) -> str\nfn cheap(x: float) -> bool\n"},
                                {"plugin.qz", R"(fn run() -> str {
    return "{shout("ab", 3)} {cheap(0.75)} {cheap(0.25)}"
}
)"}};
    quartz::CompileOutput out = quartz::compile(in);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    quartz::vm::Sandbox sb(out.program);
    auto wrong = quartz::bind<&cheap>(sb, "shout");
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error() == "host import 'shout' is (str, i32) -> str, the C++ function is (float) -> bool");
    REQUIRE(quartz::bind<&shout>(sb, "shout").has_value());
    REQUIRE(quartz::bind<&cheap>(sb, "cheap").has_value());
    REQUIRE(sb.start().has_value());
    auto r = sb.call(out.program->findFunction("run"), {});
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    CHECK(static_cast<const quartz::vm::Str*>(r->p)->view() == "ababab! true false");
}

TEST_CASE("a host property with a setter in the interface can be assigned") {
    quartz::SourceInput in[] = {{"host.qzi", R"(@handle
struct Player {}
impl Player {
    @property
    fn health(self) -> int
    fn setHealth(self, value: int)
}
fn player() -> Player
)"},
                                {"plugin.qz", R"(fn heal() -> int {
    let p = player()
    p.health = 10
    p.health += 5
    return p.health
}
)"}};
    quartz::CompileOutput out = quartz::compile(in);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    std::int64_t health = 1;
    quartz::vm::Sandbox sb(out.program);
    sb.bind("player", [](quartz::vm::HostCall& c, void*) {
        c.result.u = 42;
        return true;
    });
    sb.bind(
        "Player.health",
        [](quartz::vm::HostCall& c, void* user) {
            c.result.i = c.args[0].u == 42 ? *static_cast<std::int64_t*>(user) : -1;
            return true;
        },
        &health);
    sb.bind(
        "Player.setHealth",
        [](quartz::vm::HostCall& c, void* user) {
            *static_cast<std::int64_t*>(user) = c.args[1].i;
            return c.args[0].u == 42;
        },
        &health);
    REQUIRE(sb.start().has_value());
    CHECK(callInt(sb, "heal") == 15);
    CHECK(health == 15);
    std::string iface = in[0].text + "impl Player {\n    @property\n    fn speed(self) -> int\n}\n";
    quartz::SourceInput readOnly[] = {{"host.qzi", iface}, {"plugin.qz", "fn f() {\n    player().speed = 2\n}\n"}};
    quartz::CompileOutput bad = quartz::compile(readOnly);
    CHECK(bad.diagnostics.find("host property 'speed' is read-only; the interface needs 'fn setSpeed(self, value: "
                               "int)'") != std::string::npos);
}

TEST_CASE("a wait can produce a list the host builds") {
    quartz::SourceInput in[] = {{"host.qzi", "@handle\n@awaitable([str])\nstruct Names {}\nfn names() -> Names\n"},
                                {"plugin.qz", R"(suspend fn greet() -> str {
    let got = wait names()
    return "{got.len()} {got.join("+")}"
}
)"}};
    quartz::CompileOutput out = quartz::compile(in);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    quartz::vm::Sandbox sb(out.program);
    sb.bind("names", [](quartz::vm::HostCall& c, void*) {
        c.result.u = 1;
        return true;
    });
    REQUIRE(sb.start().has_value());
    auto t = sb.spawn(out.program->findFunction("greet"), {});
    REQUIRE(t.has_value());
    REQUIRE(t->kind == quartz::vm::TaskOutcome::Kind::Wait);
    quartz::vm::List* list = sb.hostList(sb.waitType(t->task));
    REQUIRE(list != nullptr);
    for (const char* name : {"ana", "bo"}) {
        quartz::vm::Slot s{};
        s.p = sb.newStr(name);
        REQUIRE(sb.hostListPush(list, std::span<const quartz::vm::Slot>(&s, 1)));
    }
    quartz::vm::Slot value{};
    value.p = list;
    auto done = sb.resume(t->task, value);
    REQUIRE_MESSAGE(done.has_value(), (done ? "" : done.error().describe()));
    REQUIRE(done->kind == quartz::vm::TaskOutcome::Kind::Done);
    CHECK(static_cast<const quartz::vm::Str*>(done->value.p)->view() == "2 ana+bo");
}

TEST_CASE("structs, lists and enums cross linked packages as deep copies") {
    quartz::SourceInput shopSrc[] = {{"shop.qz", R"(package org.example.shop
pub struct Order {
    item: str
    qty: int
}
pub enum Grade {
    Small
    Big(int)
}
pub struct Summary {
    total: int
    items: [str]
    largest: Order?
    grade: Grade
}
pub fn summarize(orders: [Order]) -> Summary {
    var total = 0
    var items: [str] = []
    var largest: Order? = null
    for o in orders {
        total = total + o.qty
        items.push(o.item)
        if o.qty > (largest?.qty ?? 0) {
            largest = o
        }
    }
    return Summary(total, items, largest, if total > 5 { Grade.Big(total) } else { Grade.Small })
}
)"}};
    auto manifest =
        quartz::parseManifest("name = \"org.example.shop\"\nversion = \"1.0.0\"\nexports = [\"summarize\"]\n");
    REQUIRE(manifest.has_value());
    quartz::CompileOutput shop = quartz::compile(shopSrc);
    REQUIRE_MESSAGE(shop.program, shop.diagnostics);
    auto iface = quartz::interfaceOf(*shop.program, *manifest);
    REQUIRE_MESSAGE(iface.has_value(), (iface ? "" : iface.error()));
    quartz::SourceInput clientSrc[] = {{"shop.qzi", *iface}, {"client.qz", R"(import org.example.shop.*
fn run() -> str {
    let s = summarize([Order("pear", 2), Order("fig", 5)])
    let g = match s.grade {
        Big(n) => "big {n}"
        Small => "small"
    }
    return "{s.total} {s.items.join(",")} {s.largest?.item ?? "-"} {g}"
}
)"}};
    quartz::CompileOutput client = quartz::compile(clientSrc);
    REQUIRE_MESSAGE(client.program, client.diagnostics, *iface);
    quartz::vm::Sandbox provider(shop.program);
    quartz::vm::Sandbox user(client.program);
    REQUIRE(user.link("org.example.shop", provider).has_value());
    REQUIRE(provider.start().has_value());
    REQUIRE(user.start().has_value());
    auto r = user.call(client.program->findFunction("run"), {});
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    CHECK(static_cast<const quartz::vm::Str*>(r->p)->view() == "7 pear,fig fig big 7");
}

TEST_CASE("a wait can produce a struct, a map and a nullable tuple the host builds") {
    quartz::SourceInput in[] = {{"host.qzi", R"(enum Kind {
    Plain
    Rated(int, str)
}
struct Reply {
    name: str
    tags: [str]
    score: int?
    kind: Kind
}
@handle
@awaitable(Reply)
struct Ask {}
@handle
@awaitable(Map::<str, (int, str)>)
struct Scores {}
@handle
@awaitable((int, str)?)
struct Maybe {}
fn ask() -> Ask
fn scores() -> Scores
fn maybe() -> Maybe
)"},
                                {"plugin.qz", R"(fn noise() -> str {
    var s = ""
    for i in 0..300 {
        s = s + "zzzzzzzz"
    }
    return s
}
suspend fn talk() -> str {
    let r = wait ask()
    let m = wait scores()
    let p = wait maybe()
    let k = match r.kind {
        Rated(n, why) => "{n}:{why}"
        Plain => "plain"
    }
    var parts: [str] = []
    for name, v in m {
        parts.push("{name}={v.0}/{v.1}")
    }
    return "{r.name} {r.tags.join("+")} {r.score ?? -1} {k} {parts.join(",")} {(p ?? (0, "-")).1}"
}
)"}};
    quartz::CompileOutput out = quartz::compile(in);
    REQUIRE_MESSAGE(out.program, out.diagnostics);
    const quartz::vm::Layouts& lay = *out.program->layouts;
    const quartz::ir::TypeTable& types = out.program->module->types;
    quartz::vm::Sandbox sb(out.program);
    for (const char* name : {"ask", "scores", "maybe"}) {
        sb.bind(name, [](quartz::vm::HostCall& c, void*) {
            c.result.u = 1;
            return true;
        });
    }
    REQUIRE(sb.start().has_value());
    using quartz::vm::Slot;
    auto str = [&](const char* s) {
        Slot v{};
        v.p = sb.newStr(s);
        return v;
    };
    auto noise = [&] { REQUIRE(sb.call(out.program->findFunction("noise"), {}).has_value()); };
    auto t = sb.spawn(out.program->findFunction("talk"), {});
    REQUIRE(t.has_value());
    noise();
    quartz::ir::TypeId reply = sb.waitType(t->task);
    REQUIRE(types.kind(reply) == quartz::ir::TypeKind::Struct);
    CHECK(!sb.resume(t->task, str("short")).has_value());
    std::vector<Slot> r(lay.slots(reply));
    r[lay.fieldOffset(reply, 0)] = str("ana");
    quartz::vm::List* tags = sb.hostList(types.info(reply).fields[1].type);
    for (const char* tag : {"x", "y"}) {
        Slot s = str(tag);
        REQUIRE(sb.hostListPush(tags, std::span<const Slot>(&s, 1)));
    }
    r[lay.fieldOffset(reply, 1)].p = tags;
    std::uint32_t score = lay.fieldOffset(reply, 2);
    r[score].u = 1;
    r[score + 1].i = 7;
    std::uint32_t kind = lay.fieldOffset(reply, 3);
    r[kind].u = 1;
    r[kind + 1].i = 5;
    r[kind + 2] = str("good");
    t = sb.resume(t->task, r);
    REQUIRE_MESSAGE(t.has_value(), (t ? "" : t.error().describe()));
    noise();
    quartz::vm::Map* m = sb.hostMap(sb.waitType(t->task));
    REQUIRE(m != nullptr);
    for (auto [name, n, s] : {std::tuple{"ana", 1, "a"}, {"bo", 2, "b"}, {"cy", 3, "c"}, {"ana", 10, "A"}}) {
        Slot key = str(name);
        Slot value[2] = {{}, str(s)};
        value[0].i = n;
        REQUIRE(sb.hostMapSet(m, std::span<const Slot>(&key, 1), value));
    }
    Slot mv{};
    mv.p = m;
    t = sb.resume(t->task, mv);
    REQUIRE_MESSAGE(t.has_value(), (t ? "" : t.error().describe()));
    noise();
    Slot maybe[3] = {{}, {}, str("n")};
    maybe[0].u = 1;
    maybe[1].i = 9;
    auto done = sb.resume(t->task, maybe);
    REQUIRE_MESSAGE(done.has_value(), (done ? "" : done.error().describe()));
    REQUIRE(done->kind == quartz::vm::TaskOutcome::Kind::Done);
    CHECK(static_cast<const quartz::vm::Str*>(done->value.p)->view() == "ana x+y 7 5:good ana=10/A,bo=2/b,cy=3/c n");
}

TEST_CASE("maps cross linked packages, struct keys included") {
    quartz::SourceInput shopSrc[] = {{"shop.qz", R"(package org.example.stock
@derive(Eq, Hash)
pub struct Spot {
    x: int
    y: int
}
pub fn tally(words: [str]) -> Map<str, int> {
    var m: Map<str, int> = Map()
    for w in words {
        m[w] = (m.get(w) ?? 0) + 1
    }
    m.remove("skip")
    return m
}
pub fn where(spots: Map<Spot, str>, x: int) -> str {
    return spots.get(Spot(x, x)) ?? "none"
}
)"}};
    auto manifest =
        quartz::parseManifest("name = \"org.example.stock\"\nversion = \"1.0.0\"\nexports = [\"tally\", \"where\"]\n");
    REQUIRE(manifest.has_value());
    quartz::CompileOutput shop = quartz::compile(shopSrc);
    REQUIRE_MESSAGE(shop.program, shop.diagnostics);
    auto iface = quartz::interfaceOf(*shop.program, *manifest);
    REQUIRE_MESSAGE(iface.has_value(), (iface ? "" : iface.error()));
    quartz::SourceInput clientSrc[] = {{"stock.qzi", *iface}, {"client.qz", R"(import org.example.stock.*
fn run() -> str {
    let t = tally(["b", "skip", "a", "b"])
    var parts: [str] = []
    for k, v in t {
        parts.push("{k}{v}")
    }
    var spots: Map<Spot, str> = Map()
    spots[Spot(1, 1)] = "one"
    spots[Spot(2, 2)] = "two"
    return "{parts.join(",")} {where(spots, 2)} {where(spots, 3)}"
}
)"}};
    quartz::CompileOutput client = quartz::compile(clientSrc);
    REQUIRE_MESSAGE(client.program, client.diagnostics, *iface);
    quartz::vm::Sandbox provider(shop.program);
    quartz::vm::Sandbox user(client.program);
    REQUIRE(user.link("org.example.stock", provider).has_value());
    REQUIRE(provider.start().has_value());
    REQUIRE(user.start().has_value());
    auto r = user.call(client.program->findFunction("run"), {});
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().describe()));
    CHECK(static_cast<const quartz::vm::Str*>(r->p)->view() == "b2,a1 two none");
}
