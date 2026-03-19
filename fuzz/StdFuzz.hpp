#pragma once

#include "quartz/Quartz.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

namespace quartz::fuzz {

/// Feeds the input as a `str` to `run` in a sandbox built once, so the natives see it the way a plugin would.
inline int runStd(const char* program, const std::uint8_t* data, std::size_t size) {
    static vm::Sandbox* sb = [&] {
        SourceInput in[] = {{"fuzz.qz", program}};
        CompileOutput out = compile(in);
        if (!out.program) {
            std::fprintf(stderr, "%s", out.diagnostics.c_str());
            std::abort();
        }
        vm::Limits limits;
        limits.memoryBytes = 256u << 20;
        auto* s = new vm::Sandbox(out.program, limits);
        if (!s->start()) {
            std::abort();
        }
        return s;
    }();
    vm::Slot arg{};
    arg.p = sb->newStr(std::string_view(reinterpret_cast<const char*>(data), size));
    if (arg.p != nullptr) {
        (void)sb->call(sb->program().findFunction("run"), std::span<const vm::Slot>(&arg, 1));
    }
    return 0;
}

} // namespace quartz::fuzz
