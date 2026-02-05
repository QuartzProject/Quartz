#include "vm/Sandbox.hpp"

#include <format>

namespace quartz::vm {

using ir::TypeId;
using ir::TypeKind;

bool Sandbox::copyToArena(TypeId type, const Slot* src, Slot* dst, Copy mode, std::uint32_t level) {
    const ir::TypeTable& types = program_->module->types;
    const Layouts& lay = *program_->layouts;
    const ir::TypeInfo& info = types.info(type);
    switch (info.kind) {
    case TypeKind::Str:
    case TypeKind::Bytes: {
        const auto* s = static_cast<const Str*>(src->p);
        if (s == nullptr || s->owner == Owner::Static || (mode == Copy::StateOnly && s->owner != Owner::State) ||
            (mode == Copy::NewerThan && (s->owner != Owner::Arena || s->level <= level))) {
            dst->p = src->p;
            return true;
        }
        Str* out = allocStr(s->len);
        if (out == nullptr) {
            return false;
        }
        std::memcpy(out->data, s->data, s->len);
        out->hash = s->hash;
        dst->p = out;
        return true;
    }
    case TypeKind::List: {
        auto* l = static_cast<List*>(src->p);
        if (l == nullptr) {
            dst->p = nullptr;
            return true;
        }
        if (mode == Copy::NewerThan && (l->owner != Owner::Arena || l->level <= level)) {
            dst->p = l;
            return true;
        }
        if (mode == Copy::StateOnly && l->owner != Owner::State) {
            dst->p = l;
            if (l->elemHasRefs) {
                for (std::uint32_t i = 0; i < l->len; ++i) {
                    Slot* e = l->data + static_cast<std::size_t>(i) * l->elemSlots;
                    if (!copyToArena(l->elemType, e, e, mode, level)) {
                        return false;
                    }
                }
            }
            return true;
        }
        List* out = newList(l->elemType);
        if (out == nullptr || (l->len > 0 && !growList(out, l->len))) {
            return false;
        }
        if (!l->elemHasRefs) {
            if (l->len > 0) {
                std::memcpy(out->data, l->data, static_cast<std::size_t>(l->len) * l->stride());
            }
            out->len = l->len;
            dst->p = out;
            return true;
        }
        for (std::uint32_t i = 0; i < l->len; ++i) {
            std::size_t off = static_cast<std::size_t>(i) * l->elemSlots;
            if (!copyToArena(l->elemType, l->data + off, out->data + off, mode, level)) {
                return false;
            }
            out->len = i + 1;
        }
        dst->p = out;
        return true;
    }
    case TypeKind::Map: {
        auto* m = static_cast<Map*>(src->p);
        if (m == nullptr) {
            dst->p = nullptr;
            return true;
        }
        std::uint32_t es = 1u + m->valueSlots;
        if (mode == Copy::NewerThan && (m->owner != Owner::Arena || m->level <= level)) {
            dst->p = m;
            return true;
        }
        if (mode == Copy::StateOnly && m->owner != Owner::State) {
            dst->p = m;
            for (std::uint32_t i = 0; i < m->cap; ++i) {
                if (m->hashes[i] >= 2) {
                    Slot* e = m->entries + static_cast<std::size_t>(i) * es;
                    if ((m->strKeys && !copyToArena(ir::kStr, e, e, mode, level)) ||
                        (m->valueHasRefs && !copyToArena(m->valueType, e + 1, e + 1, mode, level))) {
                        return false;
                    }
                }
            }
            return true;
        }
        const ir::TypeInfo& mi = types.info(type);
        (void)mi;
        Map* out = newMap(type);
        if (out == nullptr) {
            return false;
        }
        std::vector<Slot> tmp(es);
        for (std::uint32_t i = 0; i < m->cap; ++i) {
            if (m->hashes[i] >= 2) {
                const Slot* e = m->entries + static_cast<std::size_t>(i) * es;
                if (!copyToArena(m->keyType, e, tmp.data(), mode, level) ||
                    !copyToArena(m->valueType, e + 1, tmp.data() + 1, mode, level) ||
                    !mapSet(out, tmp.data(), tmp.data() + 1)) {
                    return false;
                }
            }
        }
        dst->p = out;
        return true;
    }
    case TypeKind::Nullable:
        if (lay.nullableIsPointer(type)) {
            if (src->p == nullptr) {
                dst->p = nullptr;
                return true;
            }
            return copyToArena(info.elem, src, dst, mode, level);
        }
        dst[0] = src[0];
        return src[0].u == 0 || copyToArena(info.elem, src + 1, dst + 1, mode, level);
    case TypeKind::Struct:
    case TypeKind::Tuple: {
        std::size_t count = info.kind == TypeKind::Struct ? info.fields.size() : info.elems.size();
        for (std::uint32_t i = 0; i < count; ++i) {
            TypeId ft = info.kind == TypeKind::Struct ? info.fields[i].type : info.elems[i];
            std::uint32_t off = lay.fieldOffset(type, i);
            if (!copyToArena(ft, src + off, dst + off, mode, level)) {
                return false;
            }
        }
        return true;
    }
    case TypeKind::Enum: {
        dst[0] = src[0];
        auto tag = static_cast<std::size_t>(src[0].u);
        if (tag >= info.variants.size()) {
            return true;
        }
        std::uint32_t off = 1;
        for (TypeId p : info.variants[tag].payload) {
            if (!copyToArena(p, src + off, dst + off, mode, level)) {
                return false;
            }
            off += lay.slots(p);
        }
        return true;
    }
    default:
        if (src != dst) {
            std::memcpy(dst, src, lay.slots(type) * sizeof(Slot));
        }
        return true;
    }
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
    Status st = execute(fn, pc, R, stack_.data(), entryDepth);
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
        return TaskOutcome{TaskOutcome::Kind::Done, 0, *r};
    }
    if (!started_) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "sandbox not started", {}, 0});
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
        if (program_->layouts->slots(p) != 1 || next >= args.size()) {
            ok = false;
            break;
        }
        stack_[slot] = args[next++];
        ok = copyToArena(p, &stack_[slot], &stack_[slot], Copy::All);
        ++slot;
    }
    resetRegions(&arena_);
    if (!ok || next != args.size()) {
        retired_.push_back(std::move(task.arena));
        task.live = false;
        return std::unexpected(trapped_ ? trap_ : Trap{ir::TrapCode::HostError, "argument mismatch", fn.name, 0});
    }
    std::memset(stack_.data() + fn.paramSlots, 0, (fn.frameSlots - fn.paramSlots) * sizeof(Slot));
    fuel_ = limits_.fuelPerCall;
    auto r = runTask(id, &fn, fn.code.data(), stack_.data(), calls_.size());
    heap_.flush();
    return r;
}

std::expected<TaskOutcome, Trap> Sandbox::resume(std::uint32_t id, Slot value) {
    if (id >= tasks_.size() || !tasks_[id].live) {
        return std::unexpected(Trap{ir::TrapCode::HostError, "no such task", {}, 0});
    }
    arena_.reset();
    hostArena_ ^= 1;
    hostArenas_[hostArena_ ^ 1].reset();
    retired_.clear();
    Task& task = tasks_[id];
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
    if (task.waitDst != 0xFFFF) {
        R[task.waitDst] = value;
        cur_ = task.arena.get();
        curLevel_ = 0;
        bool ok = copyToArena(task.waitType, &R[task.waitDst], &R[task.waitDst], Copy::All);
        cur_ = &arena_;
        if (!ok) {
            calls_.resize(entryDepth);
            return std::unexpected(trap_);
        }
    }
    fuel_ = limits_.fuelPerCall;
    auto r = runTask(id, fn, fn->code.data() + task.pc, R, entryDepth);
    heap_.flush();
    return r;
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
    }
}

std::size_t Sandbox::liveTasks() const {
    std::size_t n = 0;
    for (const Task& t : tasks_) {
        n += t.live ? 1 : 0;
    }
    return n;
}

} // namespace quartz::vm
