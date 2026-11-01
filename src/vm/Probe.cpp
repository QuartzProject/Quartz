#include "support/Utf8.hpp"
#include "vm/Sandbox.hpp"

#include <cstring>

namespace quartz::vm {

namespace {

std::uint32_t lineAt(const CompiledFunction& f, std::uint32_t pc) { return pc < f.lines.size() ? f.lines[pc] : 0; }

} // namespace

void Sandbox::setProbe(ProbeFn fn, void* user) {
    probe_ = fn;
    probeUser_ = user;
}

std::vector<FrameView> Sandbox::frames() const {
    std::vector<FrameView> out;
    if (probeFn_ == nullptr) {
        return out;
    }
    const CompiledFunction* base = program_->functions.data();
    out.push_back({static_cast<std::uint32_t>(probeFn_ - base), probePc_, lineAt(*probeFn_, probePc_), probeR_});
    for (std::size_t i = calls_.size(); i > 0; --i) {
        const CallRecord& r = calls_[i - 1];
        auto at = static_cast<std::uint32_t>(r.pc - r.fn->code.data());
        std::uint32_t pc = at > 0 ? at - 1 : 0;
        out.push_back({static_cast<std::uint32_t>(r.fn - base), pc, lineAt(*r.fn, pc), r.base});
    }
    return out;
}

std::optional<std::uint32_t> Sandbox::probeTask() const {
    return probeTask_ == kNoTask ? std::nullopt : std::optional<std::uint32_t>(probeTask_);
}

std::vector<FrameView> Sandbox::taskFrames(std::uint32_t task) const {
    std::vector<FrameView> out;
    if (task >= tasks_.size() || !tasks_[task].live || tasks_[task].native != nullptr || task == probeTask_) {
        return out;
    }
    const Task& t = tasks_[task];
    const CompiledFunction* base = program_->functions.data();
    auto add = [&](std::uint32_t function, std::uint32_t pc, std::uint32_t at) {
        const CompiledFunction& f = base[function];
        std::uint32_t before = pc > 0 ? pc - 1 : 0;
        if (static_cast<std::size_t>(at) + f.frameSlots <= t.slots.size()) {
            out.push_back({function, before, lineAt(f, before), t.slots.data() + at});
        }
    };
    add(t.function, t.pc, t.base);
    for (std::size_t i = t.frames.size(); i > 0; --i) {
        add(t.frames[i - 1].function, t.frames[i - 1].pc, t.frames[i - 1].base);
    }
    return out;
}

Str* Sandbox::probeStr(std::string_view text) {
    if (probeFn_ == nullptr || regions_.empty() || text.size() > 0x7FFFFFFF || !validUtf8(text)) {
        return nullptr;
    }
    // Level 0 outlives every `scope` open now, so the text cannot be freed under the frame that holds it.
    auto* s = static_cast<Str*>(regions_[0]->allocate(kStrHeader + text.size() + 1));
    if (s == nullptr) {
        return nullptr;
    }
    s->len = static_cast<std::uint32_t>(text.size());
    s->hash = 0;
    s->owner = Owner::Arena;
    s->level = 0;
    if (!text.empty()) {
        std::memcpy(s->data, text.data(), text.size());
    }
    s->data[text.size()] = '\0';
    return s;
}

Sandbox::Status Sandbox::probedExecute(const CompiledFunction* fn, const Ins* pc, Slot* R, Slot* base,
                                       std::size_t entryDepth) {
    auto index = static_cast<std::uint32_t>(fn - program_->functions.data());
    probeFn_ = fn;
    probePc_ = static_cast<std::uint32_t>(pc - fn->code.data());
    probeR_ = R;
    ProbeEvent begin{ProbeEvent::Kind::Begin, index, lineAt(*fn, probePc_), calls_.size()};
    bool go = probe_(*this, begin, probeUser_);
    probeFn_ = nullptr;
    if (!go) {
        fail(ir::TrapCode::HostError, "stopped by the debugger");
        return Status::Trapped;
    }
    const CompiledFunction* savedFn = probeRootFn_;
    Slot* savedR = probeRootR_;
    // A resumed task brings its outer frames back into calls_; the bottom one is the root.
    bool resumed = calls_.size() > entryDepth;
    probeRootFn_ = resumed ? calls_[entryDepth].fn : fn;
    probeRootR_ = resumed ? calls_[entryDepth].base : R;
    Status st = execute(fn, pc, R, base, entryDepth);
    probeRootFn_ = savedFn;
    probeRootR_ = savedR;
    if (probe_ != nullptr) {
        probe_(*this, {ProbeEvent::Kind::End, index, 0, entryDepth}, probeUser_);
    }
    return st;
}

bool Sandbox::probeHit(const Slot* R, std::uint32_t site) {
    if (probe_ == nullptr || site >= program_->probeSites.size()) {
        return true;
    }
    const ProbeSite& s = program_->probeSites[site];
    probeFn_ = &program_->functions[s.function];
    probePc_ = s.pc;
    probeR_ = R;
    ProbeEvent e{static_cast<ProbeEvent::Kind>(s.kind), s.function, s.line, calls_.size(), s.column};
    bool go = probe_(*this, e, probeUser_);
    probeFn_ = nullptr;
    return go || fail(ir::TrapCode::HostError, "stopped by the debugger");
}

// The trap path keeps no frame register (it cost every handler): rebuild the frame from the last call or the root.
void Sandbox::probeTrap(std::size_t entryDepth) {
    const CompiledFunction* fn = probeRootFn_;
    Slot* R = probeRootR_;
    if (calls_.size() > entryDepth) {
        const CallRecord& r = calls_.back();
        const Ins& call = *(r.pc - 1);
        R = r.base + call.c;
        if (call.op == Opc::Call) {
            fn = &program_->functions[call.b];
        } else if (const auto* c = static_cast<const Closure*>(r.base[call.b].p); c != nullptr) {
            fn = &program_->functions[c->fn];
        }
    }
    if (fn == nullptr || R == nullptr) {
        return;
    }
    probeFn_ = fn;
    probeR_ = R;
    probePc_ = 0;
    for (std::uint32_t pc = 0; pc < fn->lines.size(); ++pc) {
        if (fn->lines[pc] == trap_.line) {
            probePc_ = pc;
            break;
        }
    }
    ProbeEvent e{ProbeEvent::Kind::Trap, static_cast<std::uint32_t>(fn - program_->functions.data()), trap_.line,
                 calls_.size()};
    probe_(*this, e, probeUser_);
    probeFn_ = nullptr;
}

} // namespace quartz::vm
