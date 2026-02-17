#include "vm/Sandbox.hpp"

#include "std/Std.hpp"

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
    for (const ir::HostImport& imp : m.imports) {
        if (imp.name == "std.data.parseJson" && m.types.isResult(imp.result)) {
            ir::TypeId value = m.types.info(imp.result).variants[0].payload[0];
            const ir::TypeInfo& vi = m.types.info(value);
            if (vi.variants.size() == 7) {
                dataTypes_->value = value;
                dataTypes_->list = vi.variants[5].payload[0];
                dataTypes_->table = vi.variants[6].payload[0];
                dataTypes_->valueSlots = program_->layouts->slots(value);
            }
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

std::expected<void, std::string> Sandbox::link(std::string_view package, Sandbox& provider) {
    if (&provider == this) {
        return std::unexpected("a sandbox cannot link to itself");
    }
    const ir::TypeTable& types = program_->module->types;
    const ir::TypeTable& ptypes = provider.program_->module->types;
    std::string prefix = std::string(package) + ".";
    bool any = false;
    const auto& imports = program_->module->imports;
    for (std::size_t i = 0; i < imports.size(); ++i) {
        const ir::HostImport& imp = imports[i];
        if (!imp.name.starts_with(prefix)) {
            continue;
        }
        std::string name = imp.name.substr(prefix.size());
        std::uint32_t f = provider.program_->findFunction(name);
        if (f == ~0U || !provider.program_->functions[f].exported || provider.program_->functions[f].isSuspend) {
            return std::unexpected(std::format("{} does not export '{}'", package, name));
        }
        const CompiledFunction& fn = provider.program_->functions[f];
        std::vector<ir::TypeId> hostParams;
        for (ir::TypeId p : fn.params) {
            if (ptypes.kind(p) != ir::TypeKind::State) {
                hostParams.push_back(p);
            }
        }
        // Type ids differ between programs; builtin scalar and str ids are fixed, so compare those.
        auto simple = [](ir::TypeId t) { return t <= ir::kStr; };
        bool match = hostParams.size() == imp.params.size() && fn.result == imp.result && simple(imp.result);
        for (std::size_t k = 0; match && k < hostParams.size(); ++k) {
            match = hostParams[k] == imp.params[k] && simple(imp.params[k]);
        }
        if (!match) {
            return std::unexpected(std::format("'{}' of {} does not match its interface; rebuild against it", name, package));
        }
        (void)types;
        links_.push_back(std::make_unique<LinkTarget>(LinkTarget{&provider, f, &imp}));
        bindings_[i] = {&Sandbox::linkedCall, links_.back().get()};
        any = true;
    }
    if (!any) {
        return std::unexpected(std::format("nothing imports from {}", package));
    }
    return {};
}

bool Sandbox::linkedCall(HostCall& call, void* user) {
    const auto* t = static_cast<const LinkTarget*>(user);
    std::vector<Slot> args(t->import->params.size());
    for (std::size_t k = 0; k < args.size(); ++k) {
        args[k] = call.args[k];
        if (t->import->params[k] == ir::kStr) {
            args[k].p = t->provider->newStr(static_cast<const Str*>(call.args[k].p)->view());
            if (args[k].p == nullptr) {
                call.error = "out of memory passing an argument";
                return false;
            }
        }
    }
    auto r = t->provider->call(t->function, args);
    if (!r) {
        call.error = std::format("in {}: {}", t->import->name, r.error().describe());
        return false;
    }
    call.result = *r;
    if (t->import->result == ir::kStr) {
        call.result.p = call.string(static_cast<const Str*>(r->p)->view());
        return call.result.p != nullptr;
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
        if (plan == CompiledFunction::kBadArg || next >= args.size()) {
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

} // namespace quartz::vm
