#include "Run.hpp"

#include <cstdint>
#include <emscripten/emscripten.h>
#include <format>
#include <string>

namespace quartz::wasm {

namespace {

// clang-format off
EM_JS(void, qzp_emit, (int stream, const char* text, int length), {
    if (Module.onEmit) Module.onEmit(stream, UTF8ToString(text, length));
});
// clang-format on

struct Output {
    std::size_t bytes = 0;
    std::size_t limit = 0;
    bool overflowed = false;
};

bool hostPrint(vm::HostCall& call, void* user) {
    auto* out = static_cast<Output*>(user);
    std::string_view text = static_cast<const vm::Str*>(call.args[0].p)->view();
    out->bytes += text.size() + 1;
    if (out->limit != 0 && out->bytes > out->limit) {
        out->overflowed = true;
        call.error = "the output limit was reached";
        return false;
    }
    emit(kPrint, text);
    return true;
}

bool hostTicks(vm::HostCall& call, void*) {
    call.result.u = call.args[0].u;
    return true;
}

} // namespace

void emit(Stream stream, std::string_view text) { qzp_emit(stream, text.data(), static_cast<int>(text.size())); }

Outcome run(const std::shared_ptr<const vm::Program>& program, std::string_view entry, double fuelSlice,
            double maxSlices, double outputLimit) {
    vm::Limits limits;
    limits.fuelPerCall = static_cast<std::int64_t>(fuelSlice);
    limits.tier = vm::TierMode::Interpreter;
    vm::Sandbox sb(program, limits);
    Output printed{.limit = static_cast<std::size_t>(outputLimit)};
    sb.bind("print", hostPrint, &printed);
    sb.bind("int.ticks", hostTicks);
    if (auto s = sb.start(); !s) {
        emit(kTrap, s.error().describe());
        return kTrapped;
    }
    std::uint32_t fn = program->findFunction(std::string(entry));
    if (fn == ~0U) {
        emit(kTrap, std::format("no function '{}'", entry));
        return kNoEntry;
    }
    auto t = sb.spawn(fn, {});
    std::uint64_t ticks = 0;
    std::uint64_t yields = 0;
    auto cap = static_cast<std::uint64_t>(maxSlices);
    while (t && t->kind != vm::TaskOutcome::Kind::Done) {
        if (t->kind == vm::TaskOutcome::Kind::Wait) {
            ticks += t->value.u;
        } else if (t->kind == vm::TaskOutcome::Kind::Yield && ++yields >= cap) {
            sb.cancel(t->task);
            emit(kTrap, std::format("stopped: '{}' spent its fuel budget ({} slices of {} units)", entry, yields,
                                    limits.fuelPerCall));
            return kOutOfFuel;
        }
        t = sb.resume(t->task);
    }
    if (!t) {
        emit(kTrap, t.error().describe());
        return printed.overflowed ? kOutputLimit : kTrapped;
    }
    if (ticks > 0 || yields > 0) {
        emit(kStatus,
             std::format("[{} ticks waited, {} fuel yields, {} bytes in use]", ticks, yields, sb.memoryUsed()));
    }
    if (program->functions[fn].result == ir::kI64) {
        emit(kStatus, std::format("{} returned {}", entry, t->value.i));
    }
    return kDone;
}

} // namespace quartz::wasm
