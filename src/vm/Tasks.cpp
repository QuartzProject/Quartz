#include "vm/Sandbox.hpp"

#include <algorithm>
#include <format>

namespace quartz::vm {

using ir::TypeId;
using ir::TypeKind;

Closure* Sandbox::newClosure(std::uint32_t function, std::uint32_t captures, std::uint32_t slots, const Slot* values) {
    auto* c = static_cast<Closure*>(cur_->allocate(kClosureHeader + slots * sizeof(Slot)));
    if (c == nullptr) {
        fail(ir::TrapCode::OutOfMemory);
        return nullptr;
    }
    c->fn = function;
    c->captures = static_cast<std::uint16_t>(captures);
    c->slots = static_cast<std::uint16_t>(slots);
    c->owner = Owner::Arena;
    c->level = static_cast<std::uint8_t>(curLevel_);
    if (slots != 0) {
        std::memcpy(c->caps, values, slots * sizeof(Slot));
    }
    return c;
}

std::expected<TaskOutcome, Trap> Sandbox::runTask(std::uint32_t id, const CompiledFunction* fn, const Ins* pc, Slot* R,
                                                  std::size_t entryDepth) {
    Task& task = tasks_[id];
    taskMode_ = true;
    resetRegions(task.arena.get());
    for (auto& sc : task.scopes) {
        regions_.push_back(sc.get());
        scopeArenas_.push_back(std::move(sc));
    }
    task.scopes.clear();
    cur_ = regions_.back();
    curLevel_ = static_cast<std::uint32_t>(regions_.size() - 1);
    inCall_ = true;
    Status st = probe_ == nullptr ? execute(fn, pc, R, stack_.data(), entryDepth)
                                  : probedExecute(fn, pc, R, stack_.data(), entryDepth);
    inCall_ = false;
    InvocationArena* savedCur = cur_;
    std::uint32_t savedLevel = curLevel_;
    cur_ = regions_[0]; // relocated copies must outlive every open scope
    curLevel_ = 0;
    TaskOutcome out;
    out.task = id;
    if (st == Status::Suspended) {
        // Views into state memory must not outlive this invocation (D27): give the task its own copies.
        auto relocate = [&](const CompiledFunction* f, Slot* base) {
            for (const auto& [off, type] : f->refSlots) {
                if (!copyToArena(type, base + off, base + off, Copy::StateOnly)) {
                    return false;
                }
            }
            return true;
        };
        bool ok = relocate(suspendFn_, suspendR_);
        for (std::size_t i = entryDepth; ok && i < calls_.size(); ++i) {
            ok = relocate(calls_[i].fn, calls_[i].base);
        }
        if (!ok) {
            st = Status::Trapped;
        }
    }
    cur_ = savedCur;
    curLevel_ = savedLevel;
    taskMode_ = false;
    if (st == Status::Suspended) {
        task.scopes = takeScopes();
    }
    resetRegions(&arena_);
    const Program& P = *program_;
    auto fnIndex = [&](const CompiledFunction* f) { return static_cast<std::uint32_t>(f - P.functions.data()); };
    if (st == Status::Suspended) {
        task.frames.clear();
        for (std::size_t i = entryDepth; i < calls_.size(); ++i) {
            const CallRecord& r = calls_[i];
            task.frames.push_back({fnIndex(r.fn), static_cast<std::uint32_t>(r.pc - r.fn->code.data()),
                                   static_cast<std::uint32_t>(r.base - stack_.data()), r.dst});
        }
        calls_.resize(entryDepth);
        task.function = fnIndex(suspendFn_);
        task.pc = static_cast<std::uint32_t>(suspendPc_ - suspendFn_->code.data());
        task.base = static_cast<std::uint32_t>(suspendR_ - stack_.data());
        task.waitDst = suspendDst_;
        task.waitType = suspendType_;
        task.kind = suspendKind_;
        auto end = static_cast<std::size_t>(suspendR_ + suspendFn_->frameSlots - stack_.data());
        task.slots.assign(stack_.begin(), stack_.begin() + static_cast<std::ptrdiff_t>(end));
        out.kind = suspendKind_;
        out.value = awaited_;
        return out;
    }
    retired_.push_back(std::move(task.arena));
    task.live = false;
    task.slots.clear();
    task.frames.clear();
    if (st == Status::Trapped) {
        return std::unexpected(trap_);
    }
    finishTask(id, program_->functions[task.root].resultSlots);
    out.kind = TaskOutcome::Kind::Done;
    out.value = stack_[0];
    return out;
}

std::expected<TaskOutcome, Trap> Sandbox::spawn(std::uint32_t function, std::span<const Slot> args) {
    if (function >= program_->functions.size() || !program_->functions[function].isSuspend) {
        auto r = call(function, args);
        if (!r) {
            return std::unexpected(r.error());
        }
        finishTask(0, program_->functions[function].resultSlots);
        return TaskOutcome{TaskOutcome::Kind::Done, 0, *r};
    }
    finishedTask_ = kNoTask;
    if (!started_) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "sandbox not started", {}, 0});
    }
    if (!hostValueError_.empty()) {
        return std::unexpected(Trap{ir::TrapCode::HostError, takeHostValueError(), {}, 0});
    }
    if (inCall_) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "re-entrant call into a running sandbox", {}, 0});
    }
    const CompiledFunction& fn = program_->functions[function];
    if (!fn.exported) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "no such exported function", {}, 0});
    }
    arena_.reset();
    hostArena_ ^= 1;
    hostArenas_[hostArena_ ^ 1].reset();
    mapCursor_.map = nullptr;
    retired_.clear();
    std::uint32_t id = 0;
    while (id < tasks_.size() && tasks_[id].live) {
        ++id;
    }
    if (id == tasks_.size()) {
        tasks_.emplace_back();
    }
    Task& task = tasks_[id];
    task.live = true;
    task.arena = std::make_unique<InvocationArena>(quota_);
    task.scopes.clear();
    const ir::Module& m = *program_->module;
    std::size_t next = 0;
    std::uint32_t slot = 0;
    resetRegions(task.arena.get());
    trapped_ = false;
    bool ok = true;
    std::vector<Slot*> resolved;
    resetRegions(&arena_);
    ok = resolveStates(fn, args, resolved);
    resetRegions(task.arena.get());
    for (std::size_t pi = 0; ok && pi < fn.params.size(); ++pi) {
        ir::TypeId p = fn.params[pi];
        if (m.types.kind(p) == TypeKind::State) {
            stack_[slot++].p = resolved[pi];
            continue;
        }
        const std::size_t width = program_->layouts->slots(p);
        if (width == 0 || next + width > args.size()) {
            ok = false;
            break;
        }
        std::copy_n(args.begin() + static_cast<std::ptrdiff_t>(next), width, stack_.begin() + slot);
        next += width;
        ok = copyToArena(p, &stack_[slot], &stack_[slot], Copy::All);
        slot += static_cast<std::uint32_t>(width);
    }
    resetRegions(&arena_);
    if (!ok || next != args.size()) {
        retired_.push_back(std::move(task.arena));
        task.live = false;
        return std::unexpected(trapped_ ? trap_ : Trap{ir::TrapCode::HostError, "argument mismatch", fn.name, 0});
    }
    std::memset(stack_.data() + fn.paramSlots, 0, (fn.frameSlots - fn.paramSlots) * sizeof(Slot));
    fuel_ = limits_.fuelPerCall;
    task.root = function;
    task.native = taskEntry(function);
    task.saved.clear();
    auto r = task.native != nullptr ? runNativeTask(id, nullptr)
                                    : runTask(id, &fn, fn.code.data(), stack_.data(), calls_.size());
    if (task.native == nullptr && tierMode_ == TierMode::Auto) {
        program_->tier->charge(program_, function, static_cast<std::uint64_t>(limits_.fuelPerCall - fuel_),
                               nativeCache_);
    }
    heap_.flush();
    return r;
}

NativeEntry Sandbox::taskEntry(std::uint32_t function) {
    if (tierMode_ == TierMode::Interpreter) {
        return nullptr;
    }
    NativeEntry e = program_->tier->entry(function);
    if (e == nullptr && tierMode_ == TierMode::Native) {
        e = program_->tier->compile(*program_, function, nativeCache_);
    }
    return e;
}

// Not checked by -fsanitize=function, like Sandbox::runNative: compiled code has no type prologue.
[[clang::no_sanitize("function")]] std::expected<TaskOutcome, Trap> Sandbox::runNativeTask(std::uint32_t id,
                                                                                           const Slot* resumeValue) {
    Task& task = tasks_[id];
    const CompiledFunction& fn = program_->functions[task.root];
    const Slot* stackEnd = stack_.data() + stack_.size();
    TaskOutcome out;
    out.task = id;
    if (stack_.data() + fn.frameSlots > stackEnd) {
        fail(ir::TrapCode::StackOverflow);
        cancel(id);
        return std::unexpected(trap_);
    }
    taskMode_ = true;
    resetRegions(task.arena.get());
    for (auto& sc : task.scopes) {
        regions_.push_back(sc.get());
        scopeArenas_.push_back(std::move(sc));
    }
    task.scopes.clear();
    cur_ = regions_.back();
    curLevel_ = static_cast<std::uint32_t>(regions_.size() - 1);
    inCall_ = true;
    nativeSaved_ = &task.saved;
    auto sp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    NativeCtx ctx{this,
                  fuel_,
                  stackEnd,
                  sp > limits_.nativeStackBytes ? sp - limits_.nativeStackBytes : 0,
                  limits_.maxDepth,
                  kNoTrapFunction,
                  0,
                  curLevel_,
                  -limits_.fuelPerCall,
                  resumeValue,
                  static_cast<std::uint32_t>(task.saved.size()),
                  0,
                  0,
                  0};
    std::int32_t status = task.native(stack_.data(), &ctx);
    inCall_ = false;
    nativeSaved_ = nullptr;
    fuel_ = ctx.fuel;
    taskMode_ = false;
    bool suspended = status != 0 && ctx.suspending != 0;
    if (suspended) {
        task.saved.resize(ctx.saveTop);
        task.scopes = takeScopes();
    }
    resetRegions(&arena_);
    if (suspended) {
        task.waitDst = suspendDst_;
        task.waitType = suspendType_;
        out.kind = suspendKind_;
        out.value = awaited_;
        return out;
    }
    retired_.push_back(std::move(task.arena));
    task.live = false;
    task.saved.clear();
    if (status != 0) {
        trap_.function =
            ctx.trapFunction < program_->functions.size() ? program_->functions[ctx.trapFunction].name : "";
        trap_.file = ctx.trapFunction < program_->functions.size() ? program_->functions[ctx.trapFunction].file : "";
        trap_.line = ctx.trapLine;
        return std::unexpected(trap_);
    }
    finishTask(id, fn.resultSlots);
    out.kind = TaskOutcome::Kind::Done;
    out.value = stack_[0];
    return out;
}

namespace {

[[gnu::cold, gnu::noinline]] Trap shortResume(std::size_t width, std::size_t got) {
    return {ir::TrapCode::HostError, std::format("the wait produces {} slots and resume got {}", width, got), {}, 0};
}

} // namespace


std::expected<TaskOutcome, Trap> Sandbox::resume(std::uint32_t id, std::span<const Slot> value) {
    finishedTask_ = kNoTask;
    if (id >= tasks_.size() || !tasks_[id].live) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "no such task", {}, 0});
    }
    if (!hostValueError_.empty()) {
        return std::unexpected(Trap{ir::TrapCode::HostError, takeHostValueError(), {}, 0});
    }
    const std::size_t width = tasks_[id].waitDst != 0xFFFF ? program_->layouts->slots(tasks_[id].waitType) : 0;
    if (value.size() < width) {
        return std::unexpected(shortResume(width, value.size()));
    }
    arena_.reset();
    hostArena_ ^= 1;
    hostArenas_[hostArena_ ^ 1].reset();
    mapCursor_.map = nullptr;
    retired_.clear();
    Task& task = tasks_[id];
    if (task.native == nullptr) {
        if (NativeEntry e = taskEntry(task.root); e != nullptr && migrateTask(task, e)) {
            task.native = e;
        }
    }
    trapped_ = false;
    if (task.native != nullptr) {
        if (width != 0) {
            if (resumeSlots_.size() < width) {
                resumeSlots_.resize(width);
            }
            std::copy_n(value.begin(), width, resumeSlots_.begin());
            // A list or map grows in its region's arena, so region 0 must be the task's while it is copied.
            resetRegions(task.arena.get());
            bool ok = copyToArena(task.waitType, resumeSlots_.data(), resumeSlots_.data(), Copy::All);
            resetRegions(&arena_);
            if (!ok) {
                return std::unexpected(trap_);
            }
        }
        fuel_ = limits_.fuelPerCall;
        auto r = runNativeTask(id, width != 0 ? resumeSlots_.data() : nullptr);
        heap_.flush();
        return r;
    }
    const Program& P = *program_;
    std::copy(task.slots.begin(), task.slots.end(), stack_.begin());
    std::size_t entryDepth = calls_.size();
    for (const SavedFrame& f : task.frames) {
        const CompiledFunction* cf = &P.functions[f.function];
        calls_.push_back({cf, cf->code.data() + f.pc, stack_.data() + f.base, f.dst});
    }
    const CompiledFunction* fn = &P.functions[task.function];
    Slot* R = stack_.data() + task.base;
    trapped_ = false;
    if (width != 0) {
        std::copy_n(value.begin(), width, R + task.waitDst);
        resetRegions(task.arena.get());
        bool ok = copyToArena(task.waitType, &R[task.waitDst], &R[task.waitDst], Copy::All);
        resetRegions(&arena_);
        if (!ok) {
            calls_.resize(entryDepth);
            return std::unexpected(trap_);
        }
    }
    fuel_ = limits_.fuelPerCall;
    std::uint32_t root = task.root;
    auto r = runTask(id, fn, fn->code.data() + task.pc, R, entryDepth);
    if (tierMode_ == TierMode::Auto) {
        program_->tier->charge(program_, root, static_cast<std::uint64_t>(limits_.fuelPerCall - fuel_), nativeCache_);
    }
    heap_.flush();
    return r;
}

// An interpreted task moves to compiled code at a `wait` once its root is compiled (D69): each saved frame becomes the
// record the native frame would have written at the same point. Fuel yields stay interpreted until the next wait.
bool Sandbox::migrateTask(Task& task, NativeEntry) {
    if (task.kind == TaskOutcome::Kind::Yield || task.kind == TaskOutcome::Kind::Done) {
        return false;
    }
    const Program& P = *program_;
    std::vector<Slot> saved;
    auto add = [&](std::uint32_t function, std::uint32_t pc, std::uint32_t base) {
        const CompiledFunction& f = P.functions[function];
        for (const CompiledFunction::ResumeRecord& r : f.resumeRecords) {
            if (r.resumePc != pc) {
                continue;
            }
            std::size_t n = 0;
            for (const auto& [off, slots] : r.slots) {
                if (static_cast<std::size_t>(base) + off + slots > task.slots.size()) {
                    return false;
                }
                saved.insert(saved.end(), task.slots.begin() + base + off, task.slots.begin() + base + off + slots);
                n += slots;
            }
            Slot meta;
            meta.u = static_cast<std::uint64_t>(r.state) << 32 | n;
            saved.push_back(meta);
            return true;
        }
        return false;
    };
    if (!add(task.function, task.pc, task.base)) {
        return false;
    }
    for (std::size_t i = task.frames.size(); i-- > 0;) {
        if (!add(task.frames[i].function, task.frames[i].pc, task.frames[i].base)) {
            return false;
        }
    }
    task.saved = std::move(saved);
    task.slots = {};
    task.frames = {};
    return true;
}

void Sandbox::cancel(std::uint32_t id) {
    if (id < tasks_.size() && tasks_[id].live) {
        retired_.push_back(std::move(tasks_[id].arena));
        for (auto& sc : tasks_[id].scopes) {
            retired_.push_back(std::move(sc));
        }
        tasks_[id].scopes.clear();
        tasks_[id].live = false;
        tasks_[id].slots.clear();
        tasks_[id].frames.clear();
        tasks_[id].saved.clear();
        tasks_[id].native = nullptr;
    }
}

void Sandbox::finishTask(std::uint32_t id, std::uint32_t slots) {
    finishedTask_ = id;
    finishedSlots_ = slots;
}

std::span<const Slot> Sandbox::taskResult(std::uint32_t task) const {
    return task == finishedTask_ ? stack_.first(finishedSlots_) : std::span<const Slot>{};
}

bool Sandbox::taskNative(std::uint32_t task) const {
    return task < tasks_.size() && tasks_[task].live && tasks_[task].native != nullptr;
}

std::size_t Sandbox::liveTasks() const {
    std::size_t n = 0;
    for (const Task& t : tasks_) {
        n += t.live ? 1 : 0;
    }
    return n;
}

} // namespace quartz::vm
