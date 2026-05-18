#pragma once

#include "quartz/Quartz.hpp"
#include "quartz/quartz.h"

#include <bit>
#include <memory>
#include <new>
#include <string>
#include <vector>

struct qz_program {
    std::shared_ptr<const quartz::vm::Program> program;
    std::vector<std::uint8_t> binary;
};

struct qz_call {
    quartz::vm::HostCall& call;
    qz_sandbox* owner;
    std::size_t argSlots;
};

struct qz_sandbox {
    struct Binding {
        qz_host_fn fn;
        void* user;
        qz_sandbox* owner;
        qz_host_fn_direct direct = nullptr;
        std::size_t argSlots = 0;
    };

    std::unique_ptr<quartz::vm::Sandbox> sandbox;
    std::vector<std::unique_ptr<Binding>> bindings;
    std::string error;
    qz_trap trap = QZ_TRAP_NONE;
};

namespace quartz::capi {

static_assert(sizeof(qz_slot) == sizeof(vm::Slot));
static_assert(static_cast<int>(ir::TrapCode::HostError) + 1 == QZ_TRAP_HOST_ERROR);

inline vm::Slot toVm(qz_slot s) { return std::bit_cast<vm::Slot>(s); }

inline qz_slot toC(vm::Slot s) { return std::bit_cast<qz_slot>(s); }

inline qz_task_outcome outcomeOf(const vm::TaskOutcome& o) {
    return {static_cast<qz_task_kind>(o.kind), o.task, toC(o.value)};
}

// A C caller cannot catch C++ exceptions, so every entry point turns them into an error.
template <typename F>
qz_status guarded(qz_sandbox* sb, F&& body) {
    sb->trap = QZ_TRAP_NONE;
    try {
        return body();
    } catch (const std::bad_alloc&) {
        sb->error = "out of memory";
    } catch (...) {
        sb->error = "internal error";
    }
    return QZ_ERROR;
}

inline qz_status trapped(qz_sandbox* sb, const vm::Trap& trap) {
    sb->error = trap.describe();
    sb->trap = static_cast<qz_trap>(static_cast<int>(trap.code) + 1);
    return QZ_TRAP;
}

} // namespace quartz::capi
