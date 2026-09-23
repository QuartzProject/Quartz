#include "quartz/Quartz.hpp"
#include "vm/Layout.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <format>
#include <string>
#include <thread>
#include <vector>

namespace {

using quartz::ir::TypeId;
using quartz::ir::TypeKind;
using quartz::vm::Slot;
using quartz::vm::TierMode;

constexpr std::string_view kProgram = R"(fn show(m: Map<Pt, [str]>) -> str {
    var out = ""
    for k, v in m {
        out += "{k}:{v};"
    }
    return out
}

suspend fn main() -> (int, str, Shape?) {
    var n = 0
    var total = 0
    var last = ""
    var lastShape: Shape? = null
    while n < 3 {
        let info = wait askInfo(n)
        var tags = ""
        for k, v in info.tags {
            tags += "{k}={v},"
        }
        let spots = wait askMap(n)
        var spin = 0
        while spin < 50 {
            spin += 1
        }
        let shape = wait askShape(n)
        print("{info.name} {info.shape} {tags} {info.pos} {info.pair.0}/{info.pair.1} {show(spots)} {shape}")
        total += info.tags.len() + spots.len()
        last = info.name
        lastShape = shape
        n += 1
    }
    return (total, last, lastShape)
}
)";

constexpr std::string_view kHost = R"(@derive(Eq, Hash, Show)
struct Pt {
    x: int
    y: int
}
@derive(Show)
enum Shape {
    Dot
    Line(int)
    Box(str, [int])
}
struct Info {
    name: str
    shape: Shape
    tags: Map<str, int>
    pos: Pt?
    pair: (bool, str)
}

fn print(text: str)
@handle
@awaitable(Info)
struct AskInfo {}
@handle
@awaitable(Map::<Pt, [str]>)
struct AskMap {}
@handle
@awaitable(Shape?)
struct AskShape {}
fn askInfo(n: int) -> AskInfo
fn askMap(n: int) -> AskMap
fn askShape(n: int) -> AskShape
)";

bool logLine(quartz::vm::HostCall& call, void* user) {
    *static_cast<std::string*>(user) += std::string(static_cast<const quartz::vm::Str*>(call.args[0].p)->view()) + "\n";
    return true;
}

bool handleOf(quartz::vm::HostCall& call, void*) {
    call.result.u = static_cast<std::uint64_t>(call.args[0].i) + 1;
    return true;
}

// Builds a value of any type the host may hand to `resume` from a counter, the same way in every tier.
class Builder {
public:
    explicit Builder(quartz::vm::Sandbox& sb)
        : sb_(sb), types_(sb.program().module->types), lay_(*sb.program().layouts) {}

    void build(TypeId t, Slot* out) {
        const quartz::ir::TypeInfo& info = types_.info(t);
        int n = next_++;
        switch (info.kind) {
        case TypeKind::Str:
            out->p = sb_.newStr(std::format("s{}", n));
            return;
        case TypeKind::List: {
            quartz::vm::List* l = sb_.hostList(t);
            std::vector<Slot> e(lay_.slots(info.elem));
            for (int i = 0; i < 2; ++i) {
                build(info.elem, e.data());
                REQUIRE(sb_.hostListPush(l, e));
            }
            out->p = l;
            return;
        }
        case TypeKind::Map: {
            quartz::vm::Map* m = sb_.hostMap(t);
            std::vector<Slot> k(lay_.slots(info.key));
            std::vector<Slot> v(lay_.slots(info.elem));
            for (int i = 0; i < 3; ++i) {
                build(info.key, k.data());
                build(info.elem, v.data());
                REQUIRE(sb_.hostMapSet(m, k, v));
            }
            out->p = m;
            return;
        }
        case TypeKind::Struct:
        case TypeKind::Tuple: {
            std::size_t count = info.kind == TypeKind::Struct ? info.fields.size() : info.elems.size();
            for (std::uint32_t i = 0; i < count; ++i) {
                build(info.kind == TypeKind::Struct ? info.fields[i].type : info.elems[i],
                      out + lay_.fieldOffset(t, i));
            }
            return;
        }
        case TypeKind::Enum: {
            std::size_t tag = static_cast<std::size_t>(n) % info.variants.size();
            out[0].u = tag;
            std::uint32_t off = 1;
            for (TypeId p : info.variants[tag].payload) {
                build(p, out + off);
                off += lay_.slots(p);
            }
            return;
        }
        case TypeKind::Nullable:
            if (lay_.nullableIsPointer(t)) {
                out->p = nullptr;
                if (n % 3 != 0) {
                    build(info.elem, out);
                }
                return;
            }
            out[0].u = n % 3 != 0 ? 1 : 0;
            build(info.elem, out + 1);
            return;
        case TypeKind::Bool:
            out->u = static_cast<std::uint64_t>(n % 2);
            return;
        default:
            out->i = n * 7;
            return;
        }
    }

private:
    quartz::vm::Sandbox& sb_;
    const quartz::ir::TypeTable& types_;
    const quartz::vm::Layouts& lay_;
    int next_ = 0;
};

std::string runWide(const std::shared_ptr<const quartz::vm::Program>& p, TierMode mode, std::int64_t fuel) {
    std::string log;
    quartz::vm::Limits limits;
    limits.tier = mode;
    limits.fuelPerCall = fuel;
    quartz::vm::Sandbox sb(p, limits);
    sb.bind("print", logLine, &log);
    for (const char* name : {"askInfo", "askMap", "askShape"}) {
        sb.bind(name, handleOf);
    }
    REQUIRE(sb.start().has_value());
    std::uint32_t f = p->findFunction("main");
    Builder builder(sb);
    bool migrated = false;
    auto t = sb.spawn(f, {});
    for (int steps = 0; t && t->kind != quartz::vm::TaskOutcome::Kind::Done && steps < 100'000; ++steps) {
        log += std::format("[{} {}]\n", static_cast<int>(t->kind), t->value.u);
        if (mode == TierMode::Auto) {
            for (int i = 0; i < 2000 && p->tier->entry(f) == nullptr; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            migrated = migrated || sb.taskNative(t->task);
        }
        TypeId type = sb.waitType(t->task);
        std::vector<Slot> value(type == quartz::ir::kVoid ? 1 : p->layouts->slots(type));
        if (type != quartz::ir::kVoid) {
            builder.build(type, value.data());
        }
        t = sb.resume(t->task, value);
    }
    if (t) {
        std::span<const Slot> r = sb.taskResult(t->task);
        REQUIRE(r.size() == 6);
        auto* last = static_cast<const quartz::vm::Str*>(r[1].p);
        log += std::format("result {} {} {} {}\n", r[0].i, last->view(), r[2].u, r[2].u != 0 ? r[3].u : 9);
    } else {
        log += t.error().describe();
    }
    if (mode == TierMode::Native) {
        REQUIRE(p->tier->entry(f) != nullptr);
    }
    if (mode == TierMode::Auto) {
        CHECK(migrated);
    }
    return log;
}

} // namespace

TEST_CASE("waits producing structs, maps, enums and nullables resume the same in both tiers") {
    auto build = [] {
        quartz::CompileOutput out = quartz::compile(
            std::vector<quartz::SourceInput>{{"wide.qz", std::string(kProgram)}, {"host.qzi", std::string(kHost)}});
        REQUIRE_MESSAGE(out.program, out.diagnostics);
        return out.program;
    };
    auto p = build();
    std::string interp = runWide(p, TierMode::Interpreter, 100'000'000);
    CHECK(interp.ends_with("result 18 s89 1 2\n"));
    CHECK(runWide(build(), TierMode::Auto, 100'000'000) == interp);
    for (std::int64_t fuel : {100'000'000, 40, 97, 1'000}) {
        CAPTURE(fuel);
        CHECK(runWide(p, TierMode::Native, fuel) == runWide(p, TierMode::Interpreter, fuel));
    }
}
