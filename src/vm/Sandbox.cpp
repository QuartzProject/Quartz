#include "vm/Sandbox.hpp"

#include "std/Std.hpp"

#include <algorithm>
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
    tierMode_ = resolveTierMode(limits_.tier);
    if (tierMode_ != TierMode::Interpreter) {
        nativeCache_ = resolveCacheDir(limits_.nativeCacheDir);
    }
    resetRegions(&arena_);
    bindNatives();
}

void Sandbox::bindNatives() {
    const ir::Module& m = *program_->module;
    dataTypes_ = std::make_unique<stdlib::DataTypes>();
    // Found by shape, not through an import: unused library imports are pruned (ir::pruneLibrary).
    static constexpr std::string_view kVariants[] = {"Null", "Bool", "Int", "Float", "Str", "List", "Table"};
    for (ir::TypeId t = 0; t < m.types.size() && dataTypes_->value == 0; ++t) {
        const ir::TypeInfo& vi = m.types.info(t);
        if (vi.kind != ir::TypeKind::Enum || vi.name != "Value" || vi.variants.size() != 7) {
            continue;
        }
        bool shaped = true;
        for (std::size_t k = 0; k < 7; ++k) {
            shaped = shaped && vi.variants[k].name == kVariants[k] && vi.variants[k].payload.size() == (k == 0 ? 0 : 1);
        }
        static constexpr ir::TypeId kScalars[] = {ir::kBool, ir::kI64, ir::kF64, ir::kStr};
        for (std::size_t k = 1; shaped && k < 5; ++k) {
            shaped = vi.variants[k].payload[0] == kScalars[k - 1];
        }
        if (!shaped) {
            continue;
        }
        const ir::TypeInfo& list = m.types.info(vi.variants[5].payload[0]);
        const ir::TypeInfo& table = m.types.info(vi.variants[6].payload[0]);
        if (list.kind == ir::TypeKind::List && list.elem == t && table.kind == ir::TypeKind::Map &&
            table.key == ir::kStr && table.elem == t) {
            dataTypes_->value = t;
            dataTypes_->list = vi.variants[5].payload[0];
            dataTypes_->table = vi.variants[6].payload[0];
            dataTypes_->valueSlots = program_->layouts->slots(t);
        }
    }
    for (const ir::HostImport& imp : m.imports) {
        for (const stdlib::Native& n : stdlib::natives()) {
            if (n.name == imp.name) {
                auto ctx = std::make_unique<stdlib::NativeCtx>(stdlib::NativeCtx{&imp, dataTypes_.get()});
                bind(n.name, n.fn, ctx.get());
                nativeCtx_.push_back(std::move(ctx));
            }
        }
    }
}

bool Sandbox::enter(std::uint32_t function, Slot* base) {
    if (tierMode_ != TierMode::Interpreter && !program_->functions[function].isSuspend) {
        TierState& tier = *program_->tier;
        NativeEntry e = tier.entry(function);
        if (e == nullptr && tierMode_ == TierMode::Native) {
            e = tier.compile(*program_, function, nativeCache_);
        }
        if (e != nullptr) {
            return runNative(e, function, base);
        }
    }
    return run(function, base);
}

bool Sandbox::runNative(NativeEntry entry, std::uint32_t function, Slot* base) {
    const Slot* stackEnd = stack_.data() + stack_.size();
    if (base + program_->functions[function].frameSlots > stackEnd) {
        return fail(ir::TrapCode::StackOverflow);
    }
    auto sp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    // The task fields stay unset: only code compiled for a task reads them.
    NativeCtx ctx;
    ctx.sandbox = this;
    ctx.fuel = fuel_;
    ctx.stackEnd = stackEnd;
    ctx.nativeStackLimit = sp > limits_.nativeStackBytes ? sp - limits_.nativeStackBytes : 0;
    ctx.maxDepth = limits_.maxDepth;
    ctx.trapFunction = kNoTrapFunction;
    ctx.trapLine = 0;
    ctx.scopeLevel = curLevel_;
    std::int32_t status = entry(base, &ctx);
    fuel_ = ctx.fuel;
    if (status != 0) {
        trap_.function = ctx.trapFunction < program_->functions.size() ? program_->functions[ctx.trapFunction].name : "";
        trap_.file = ctx.trapFunction < program_->functions.size() ? program_->functions[ctx.trapFunction].file : "";
        trap_.line = ctx.trapLine;
        return false;
    }
    return true;
}

Sandbox::~Sandbox() {
    const ir::Module& m = *program_->module;
    auto releaseInstance = [&](std::size_t i, Slot* st) {
        const ir::TypeInfo& info = m.types.info(m.states[i]);
        for (std::uint32_t f = 0; f < info.fields.size(); ++f) {
            releaseState(info.fields[f].type, st + program_->layouts->fieldOffset(m.states[i], f));
        }
        heap_.release(st);
    };
    for (std::size_t i = 0; i < states_.size(); ++i) {
        if (states_[i] != nullptr) {
            releaseInstance(i, states_[i]);
        }
    }
    for (std::size_t i = 0; i < perHandle_.size(); ++i) {
        for (auto& [handle, st] : perHandle_[i]) {
            releaseInstance(i, st);
        }
    }
    heap_.flush();
}

Slot* Sandbox::instanceFor(std::size_t state, std::uint64_t handle) {
    auto& map = perHandle_[state];
    if (auto it = map.find(handle); it != map.end()) {
        return it->second;
    }
    const ir::Module& m = *program_->module;
    std::uint32_t slots = std::max<std::uint32_t>(program_->layouts->stateSlots(m.states[state]), 1);
    auto* st = static_cast<Slot*>(heap_.allocate(slots * sizeof(Slot)));
    if (st == nullptr) {
        fail(ir::TrapCode::OutOfMemory, "per-handle state does not fit in the memory limit");
        return nullptr;
    }
    std::memset(st, 0, slots * sizeof(Slot));
    std::int64_t savedFuel = fuel_;
    bool savedInCall = inCall_;
    fuel_ = limits_.fuelPerCall;
    stack_[0].p = st;
    inCall_ = true;
    bool ok = run(m.stateInit[state], stack_.data());
    inCall_ = savedInCall;
    fuel_ = savedFuel;
    if (!ok) {
        heap_.release(st);
        return nullptr;
    }
    map.emplace(handle, st);
    return st;
}

bool Sandbox::resolveStates(const CompiledFunction& fn, std::span<const Slot> args, std::vector<Slot*>& out) {
    out.assign(fn.entryPlan.size(), nullptr);
    for (std::size_t i = 0; i < fn.entryPlan.size(); ++i) {
        std::int32_t plan = fn.entryPlan[i];
        if (plan < 0) {
            continue;
        }
        auto s = static_cast<std::size_t>(plan);
        if (fn.entryHandle[i] < 0) {
            out[i] = states_[s];
            continue;
        }
        auto h = static_cast<std::size_t>(fn.entryHandle[i]);
        if (h >= args.size()) {
            return fail(ir::TrapCode::HostError, "argument mismatch");
        }
        out[i] = instanceFor(s, args[h].u);
        if (out[i] == nullptr) {
            return false;
        }
    }
    return true;
}

void Sandbox::dropHandle(std::uint64_t handle) {
    const ir::Module& m = *program_->module;
    for (std::size_t i = 0; i < perHandle_.size(); ++i) {
        auto it = perHandle_[i].find(handle);
        if (it == perHandle_[i].end()) {
            continue;
        }
        const ir::TypeInfo& info = m.types.info(m.states[i]);
        for (std::uint32_t f = 0; f < info.fields.size(); ++f) {
            releaseState(info.fields[f].type, it->second + program_->layouts->fieldOffset(m.states[i], f));
        }
        heap_.release(it->second);
        perHandle_[i].erase(it);
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
    perHandle_.resize(m.states.size());
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        if (m.types.info(m.states[i]).persistPer != 0) {
            states_.push_back(nullptr); // instances are made per handle on demand
            continue;
        }
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
        if (states_[i] == nullptr) {
            continue;
        }
        Slot arg{};
        arg.p = states_[i];
        trapped_ = false;
        fuel_ = limits_.fuelPerCall;
        stack_[0] = arg;
        inCall_ = true;
        bool ok = enter(m.stateInit[i], stack_.data());
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
    if (inCall_) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "re-entrant call into a running sandbox", {}, 0});
    }
    if (!started_) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "sandbox not started", {}, 0});
    }
    if (function >= program_->functions.size() || !program_->functions[function].exported) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "no such exported function", {}, 0});
    }
    const CompiledFunction& fn = program_->functions[function];
    resetRegions(&arena_);
    arena_.reset();
    hostArena_ ^= 1;
    hostArenas_[hostArena_ ^ 1].reset();
    mapCursor_.map = nullptr;
    trapped_ = false;
    std::vector<Slot*>& resolved = resolved_;
    if (!resolveStates(fn, args, resolved)) {
        return std::unexpected(trap_);
    }
    std::size_t next = 0;
    std::uint32_t slot = 0;
    for (std::size_t pi = 0; pi < fn.entryPlan.size(); ++pi) {
        std::int32_t plan = fn.entryPlan[pi];
        if (plan >= 0) {
            stack_[slot++].p = resolved[pi];
            continue;
        }
        const std::size_t width = plan == CompiledFunction::kBadArg ? 0 : program_->layouts->slots(fn.params[pi]);
        if (width == 0 || next + width > args.size()) {
            return std::unexpected(Trap{ir::TrapCode::HostError, "argument mismatch", fn.name, 0});
        }
        std::copy_n(args.begin() + static_cast<std::ptrdiff_t>(next), width, stack_.begin() + slot);
        next += width;
        slot += static_cast<std::uint32_t>(width);
    }
    if (next != args.size()) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "too many arguments", fn.name, 0});
    }
    trapped_ = false;
    fuel_ = limits_.fuelPerCall;
    inCall_ = true;
    bool ok = enter(function, stack_.data());
    inCall_ = false;
    if (tierMode_ == TierMode::Auto) {
        program_->tier->charge(program_, function, static_cast<std::uint64_t>(limits_.fuelPerCall - fuel_),
                               nativeCache_);
    }
    heap_.flush();
    if (!ok) {
        return std::unexpected(trap_);
    }
    return stack_[0];
}

bool Sandbox::run(std::uint32_t function, Slot* base) {
    const CompiledFunction* fn = &program_->functions[function];
    if (fn->isSuspend) {
        fail(ir::TrapCode::HostError, "suspend functions run as tasks; use spawn");
        return false;
    }
    return execute(fn, fn->code.data(), base, base, calls_.size()) == Status::Done;
}

} // namespace quartz::vm
