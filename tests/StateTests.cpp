#include "quartz/Quartz.hpp"

#include <doctest/doctest.h>
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
