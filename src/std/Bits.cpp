#include "std/Natives.hpp"

#include <bit>
#include <limits>

namespace quartz::stdlib {

namespace {

// Quartz has one NaN (D24): a NaN read from bits becomes it.
double canonical(double d) { return d != d ? std::numeric_limits<double>::quiet_NaN() : d; }

bool floatToBits(vm::HostCall& call, void*) {
    call.result.u = std::bit_cast<std::uint64_t>(call.args[0].f);
    return true;
}

bool floatFromBits(vm::HostCall& call, void*) {
    call.result.f = canonical(std::bit_cast<double>(call.args[0].u));
    return true;
}

bool f32ToBits(vm::HostCall& call, void*) {
    call.result.u = std::bit_cast<std::uint32_t>(static_cast<float>(call.args[0].f));
    return true;
}

bool f32FromBits(vm::HostCall& call, void*) {
    call.result.f = canonical(std::bit_cast<float>(static_cast<std::uint32_t>(call.args[0].u)));
    return true;
}

} // namespace

void addBitsNatives(std::vector<Native>& out) {
    out.insert(out.end(), {
                              {"org.quartzlang.bytes.floatToBits", floatToBits},
                              {"org.quartzlang.bytes.floatFromBits", floatFromBits},
                              {"org.quartzlang.bytes.f32ToBits", f32ToBits},
                              {"org.quartzlang.bytes.f32FromBits", f32FromBits},
                          });
}

} // namespace quartz::stdlib
