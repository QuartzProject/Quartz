#include "ir/EnumProof.hpp"

#include <cstdint>

namespace quartz::ir {

namespace {

// What a register is known to hold, valid while `epoch` is current and the enum register keeps `version`.
struct Fact {
    enum class Kind : std::uint8_t { None, Const, Tag, Is, IsNot } kind = Kind::None;
    Reg e = kNoReg;
    std::uint64_t value = 0; // Const: the constant; Is, IsNot: the variant
    std::uint32_t version = 0;
    std::uint32_t epoch = 0;
};

class Prover {
public:
    Prover(const Module& m, const Function& fn)
        : m_(m), fn_(fn), facts_(fn.regs.size()), known_(fn.regs.size()), version_(fn.regs.size(), 0) {}

    std::vector<bool> run() {
        std::vector<bool> proven(fn_.code.size(), false);
        for (std::size_t pc = 0; pc < fn_.code.size(); ++pc) {
            const Inst& in = fn_.code[pc];
            switch (in.op) {
            case Op::Loop:
            case Op::Else:
            case Op::End:
                // A loop head, an else and an end are reached from other paths too: start over.
                ++epoch_;
                break;
            case Op::If:
                if (const Fact* f = get(in.a, Fact::Kind::Is)) {
                    know(f->e, f->value);
                }
                break;
            case Op::BrIf:
                if (const Fact* f = get(in.a, Fact::Kind::IsNot)) {
                    know(f->e, f->value);
                }
                break;
            case Op::EnumField:
                proven[pc] = isKnown(in.a, in.imm);
                write(in.dst);
                break;
            default:
                step(in);
                break;
            }
        }
        return proven;
    }

private:
    bool inRange(Reg r) const { return r < facts_.size(); }

    const Fact* get(Reg r, Fact::Kind kind) const {
        if (!inRange(r)) {
            return nullptr;
        }
        const Fact& f = facts_[r];
        if (f.kind != kind || f.epoch != epoch_) {
            return nullptr;
        }
        if (kind != Fact::Kind::Const && (!inRange(f.e) || version_[f.e] != f.version)) {
            return nullptr;
        }
        return &f;
    }

    void know(Reg e, std::uint64_t variant) {
        if (inRange(e)) {
            known_[e] = {Fact::Kind::Is, e, variant, version_[e], epoch_};
        }
    }

    bool isKnown(Reg e, std::uint64_t variant) const {
        if (!inRange(e)) {
            return false;
        }
        const Fact& k = known_[e];
        return k.kind == Fact::Kind::Is && k.epoch == epoch_ && k.version == version_[e] && k.value == variant;
    }

    void write(Reg r) {
        if (inRange(r)) {
            ++version_[r];
            facts_[r] = {};
        }
    }

    // Sets `dst`'s fact after its write, unless the fact is about `dst` itself (its old value is gone).
    void set(Reg dst, Fact f) {
        write(dst);
        if (inRange(dst) && f.e != dst) {
            f.epoch = epoch_;
            if (f.kind != Fact::Kind::Const) {
                f.version = version_[f.e];
            }
            facts_[dst] = f;
        }
    }

    void step(const Inst& in) {
        switch (in.op) {
        case Op::Const:
            if (in.imm < m_.constants.size() && m_.constants[in.imm].kind == ConstKind::Int) {
                set(in.dst, {Fact::Kind::Const, kNoReg, m_.constants[in.imm].bits});
                return;
            }
            break;
        case Op::EnumTag:
            if (inRange(in.a)) {
                set(in.dst, {Fact::Kind::Tag, in.a});
                return;
            }
            break;
        case Op::Eq: {
            const Fact* tag = get(in.a, Fact::Kind::Tag);
            const Fact* k = get(in.b, Fact::Kind::Const);
            if (tag == nullptr) {
                tag = get(in.b, Fact::Kind::Tag);
                k = get(in.a, Fact::Kind::Const);
            }
            bool ints = inRange(in.a) && inRange(in.b) && fn_.regs[in.a] == kI64 && fn_.regs[in.b] == kI64;
            if (tag != nullptr && k != nullptr && ints) {
                set(in.dst, {Fact::Kind::Is, tag->e, k->value});
                return;
            }
            break;
        }
        case Op::Not:
            if (const Fact* f = get(in.a, Fact::Kind::Is)) {
                set(in.dst, {Fact::Kind::IsNot, f->e, f->value});
                return;
            }
            if (const Fact* f = get(in.a, Fact::Kind::IsNot)) {
                set(in.dst, {Fact::Kind::Is, f->e, f->value});
                return;
            }
            break;
        case Op::FieldSet:
        case Op::ArraySet:
        case Op::Persist:
            write(in.a);
            break;
        case Op::ScopeExit:
            for (Reg r = in.a; r < in.b && inRange(r); ++r) {
                write(r);
            }
            break;
        default:
            break;
        }
        write(in.dst);
    }

    const Module& m_;
    const Function& fn_;
    std::vector<Fact> facts_;
    std::vector<Fact> known_;
    std::vector<std::uint32_t> version_;
    std::uint32_t epoch_ = 1;
};

} // namespace

std::vector<bool> provenEnumFields(const Module& m, const Function& fn) { return Prover(m, fn).run(); }

} // namespace quartz::ir
