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
