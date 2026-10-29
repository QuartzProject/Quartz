#include "quartz/Quartz.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace {

constexpr std::string_view kProgram = R"(@derive(Eq, Hash)
enum Chain {
    Nil
    Cons(int, Chain)
}
@derive(Eq, Hash)
struct Pt {
    x: int
    y: i8
}
struct Box {
    name: str?
    data: Bytes
    ratio: f32
    kids: [Box]
}
state S {
    chain: Chain = Chain.Nil
    byPoint: Map<Pt, [str]> = Map()
    byChain: Map<Chain, (bool, char)> = Map()
    byList: Map<[int], float> = Map()
    names: Map<str, u16> = Map()
    root: Box = Box(null, "".toBytes(), 0.0, [])
}
)";

} // namespace

// A state document comes back from disk: whatever it holds, importing fails cleanly or loads what exports again.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const auto program = [] {
        quartz::SourceInput in[] = {{"state.qz", std::string(kProgram)}};
        auto out = quartz::compile(in);
        if (!out.program) {
            std::abort();
        }
        return out.program;
    }();
    quartz::vm::Limits limits;
    limits.memoryBytes = 8u << 20;
    quartz::vm::Sandbox sb(program, limits);
    if (!sb.start()) {
        std::abort();
    }
    if (!sb.importState(std::string_view(reinterpret_cast<const char*>(data), size))) {
        return 0;
    }
    auto saved = sb.exportState();
    if (!saved || !sb.importState(*saved)) {
        std::abort();
    }
    return 0;
}
