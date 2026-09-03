#pragma once

#include "quartz/Quartz.hpp"
#include "quartz/quartz.h"

#include <bit>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

struct qz_program {
    std::shared_ptr<const quartz::vm::Program> program;
    std::vector<std::uint8_t> binary;
    // Qualified function names and type names, made on first use; a program is shared across threads.
    mutable std::once_flag namesOnce{};
    mutable std::vector<std::string> functionNames{};
    mutable std::vector<std::string> typeNames{};
    mutable std::vector<std::uint32_t> exports{};
    mutable std::vector<std::vector<const quartz::ir::Annotation*>> attributes{}; // per function
};

struct qz_call {
    quartz::vm::HostCall& call;
    qz_sandbox* owner;
    std::size_t argSlots;
};

struct qz_diagnostics {
    std::string text;
    std::vector<quartz::CompileMessage> items;
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

/// The front end behind qz_compile and qz_compile_with.
CompileOutput compileSources(const qz_source* sources, std::size_t count, const qz_compile_options* options,
                             std::vector<RuleSetting> rules);

static_assert(sizeof(qz_slot) == sizeof(vm::Slot));
static_assert(static_cast<int>(ir::TrapCode::HostError) + 1 == QZ_TRAP_HOST_ERROR);
static_assert(static_cast<int>(ir::FnVisibility::Public) == QZ_VISIBILITY_PUBLIC);
static_assert(static_cast<int>(ir::AnnotationArg::Kind::Name) == QZ_ATTRIBUTE_ARG_NAME);

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

// A str or Bytes the host could not build outside a call fails the next call, before it runs.
inline bool hostValueFailed(qz_sandbox* sb) {
    sb->error = sb->sandbox->takeHostValueError();
    return !sb->error.empty();
}

inline qz_status trapped(qz_sandbox* sb, const vm::Trap& trap) {
    sb->error = trap.describe();
    sb->trap = static_cast<qz_trap>(static_cast<int>(trap.code) + 1);
    return QZ_TRAP;
}

} // namespace quartz::capi
