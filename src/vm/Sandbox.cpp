#include "vm/Sandbox.hpp"

#include <format>

namespace quartz::vm {

Sandbox::Sandbox(std::shared_ptr<const Program> program, Limits limits)
    : program_(std::move(program)), limits_(limits), arena_(quota_), hostArenas_{InvocationArena(quota_), InvocationArena(quota_)},
      heap_(quota_) {
    quota_.limit = limits_.memoryBytes;
    stackMem_ = std::make_unique_for_overwrite<Slot[]>(limits_.stackSlots);
    stack_ = std::span<Slot>(stackMem_.get(), limits_.stackSlots);
    bindings_.resize(program_->module->imports.size());
    calls_.reserve(limits_.maxDepth + 1);
}

Sandbox::~Sandbox() {
    const ir::Module& m = *program_->module;
    for (std::size_t i = 0; i < states_.size(); ++i) {
        Slot* st = states_[i];
        if (st == nullptr) {
            continue;
        }
        const ir::TypeInfo& info = m.types.info(m.states[i]);
        for (std::uint32_t f = 0; f < info.fields.size(); ++f) {
            releaseState(info.fields[f].type, st + program_->layouts->fieldOffset(m.states[i], f));
        }
        heap_.release(st);
    }
    heap_.flush();
}

bool Sandbox::bind(std::string_view importName, HostFn fn, void* user) {
    bool found = false;
    const auto& imports = program_->module->imports;
    for (std::size_t i = 0; i < imports.size(); ++i) {
        if (imports[i].name == importName) {
            bindings_[i] = {fn, user};
            found = true;
        }
    }
    return found;
}

std::expected<void, Trap> Sandbox::start() {
    const ir::Module& m = *program_->module;
    for (std::size_t i = 0; i < bindings_.size(); ++i) {
        if (bindings_[i].fn == nullptr) {
            return std::unexpected(Trap{ir::TrapCode::HostError, std::format("host import '{}' is not bound", m.imports[i].name), {}, 0});
        }
    }
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        std::uint32_t slots = program_->layouts->stateSlots(m.states[i]);
        auto* st = static_cast<Slot*>(heap_.allocate(std::max<std::size_t>(slots, 1) * sizeof(Slot)));
        if (st == nullptr) {
            return std::unexpected(Trap{ir::TrapCode::OutOfMemory, "state does not fit in the memory limit", {}, 0});
        }
        std::memset(st, 0, std::max<std::size_t>(slots, 1) * sizeof(Slot));
        states_.push_back(st);
    }
    started_ = true;
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        Slot arg{};
        arg.p = states_[i];
        trapped_ = false;
        fuel_ = limits_.fuelPerCall;
        stack_[0] = arg;
        inCall_ = true;
        bool ok = run(m.stateInit[i], stack_.data());
        inCall_ = false;
        heap_.flush();
        if (!ok) {
            started_ = false;
            return std::unexpected(trap_);
        }
    }
    arena_.reset();
    return {};
}

std::expected<Slot, Trap> Sandbox::call(std::uint32_t function, std::span<const Slot> args) {
    if (!started_) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "sandbox not started", {}, 0});
    }
    if (function >= program_->functions.size() || !program_->functions[function].exported) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "no such exported function", {}, 0});
    }
    const CompiledFunction& fn = program_->functions[function];
    const ir::Module& m = *program_->module;
    arena_.reset();
    hostArena_ ^= 1;
    hostArenas_[hostArena_ ^ 1].reset();
    std::size_t next = 0;
    std::uint32_t slot = 0;
    for (ir::TypeId p : fn.params) {
        if (m.types.kind(p) == ir::TypeKind::State) {
            for (std::size_t s = 0; s < m.states.size(); ++s) {
                if (m.states[s] == p) {
                    stack_[slot].p = states_[s];
                }
            }
            ++slot;
            continue;
        }
        if (program_->layouts->slots(p) != 1 || next >= args.size()) {
            return std::unexpected(Trap{ir::TrapCode::HostError, "argument mismatch", fn.name, 0});
        }
        stack_[slot++] = args[next++];
    }
    if (next != args.size()) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "too many arguments", fn.name, 0});
    }
    trapped_ = false;
    fuel_ = limits_.fuelPerCall;
    inCall_ = true;
    bool ok = run(function, stack_.data());
    inCall_ = false;
    heap_.flush();
    if (!ok) {
        return std::unexpected(trap_);
    }
    return stack_[0];
}

} // namespace quartz::vm
