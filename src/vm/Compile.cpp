#include "ir/Builtins.hpp"
#include "ir/Liveness.hpp"
#include "vm/Bytecode.hpp"
#include "vm/Tier.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace quartz::vm {

using ir::Inst;
using ir::Intrinsic;
using ir::Op;
using ir::Reg;
using ir::TypeId;
using ir::TypeKind;

namespace {

class FunctionCompiler {
public:
    FunctionCompiler(Program& prog, const ir::Function& fn) : prog_(prog), fn_(fn), lay_(*prog.layouts) {}

    CompiledFunction run() {
        CompiledFunction out;
        out.name = fn_.name;
        out.exported = fn_.exported;
        out.result = fn_.result;
        out.resultSlots = lay_.slots(fn_.result);
        std::uint32_t next = 0;
        offset_.resize(fn_.regs.size());
        for (std::size_t r = 0; r < fn_.regs.size(); ++r) {
            offset_[r] = next;
            next += lay_.slots(fn_.regs[r]);
            if (r + 1 == fn_.paramCount) {
                out.paramSlots = next;
            }
        }
        for (std::uint32_t p = 0; p < fn_.paramCount; ++p) {
            out.params.push_back(fn_.regs[p]);
        }
        regSlots_ = next;
        origOffset_ = offset_;
        if (fn_.isSuspend) {
            live_ = std::make_unique<ir::Liveness>(fn_);
        }
        analyze();
        for (pc_ = 0; pc_ < fn_.code.size(); ++pc_) {
            if (skip_ > 0) {
                --skip_;
                continue;
            }
            line_ = fn_.code[pc_].line;
            const Inst& in = fn_.code[pc_];
            retarget_ = ir::kNoReg;
            retargetFrom_ = ir::kNoReg;
            if (fuse && in.dst != ir::kNoReg && canRetarget(in) && pc_ + 1 < fn_.code.size()) {
                const Inst& mv = fn_.code[pc_ + 1];
                if (mv.op == Op::Move && mv.a == in.dst && uses_[in.dst] == 1 && defs_[in.dst] == 1 &&
                    defs_[mv.dst] > 1 && n(mv.dst) == n(in.dst)) {
                    retarget_ = mv.dst;
                    retargetFrom_ = in.dst;
                    offset_[in.dst] = offset_[mv.dst];
                    skip_ = 1;
                }
            }
            lower(in);
        }
        out.frameSlots = regSlots_ + maxOutgoing_;
        out.argBase = regSlots_;
        if (out.frameSlots > 0xFFFF) {
            out.frameSlots = 0xFFFF; // the compiler rejects such functions below
            tooBig_ = true;
        }
        out.isSuspend = fn_.isSuspend;
        if (fn_.isSuspend) {
            for (std::size_t r = 0; r < fn_.regs.size(); ++r) {
                if (prog_.module->types.hasRefs(fn_.regs[r])) {
                    std::pair<std::uint32_t, TypeId> e{offset_[r], fn_.regs[r]};
                    if (std::find(out.refSlots.begin(), out.refSlots.end(), e) == out.refSlots.end()) {
                        out.refSlots.push_back(e);
                    }
                }
            }
        }
        out.code = std::move(code_);
        out.lines = std::move(lines_);
        out.file = fn_.file;
        out.loopLines = std::move(loopLines_);
        out.resumeRecords = std::move(resumeRecords_);
        if (tooBig_) {
            out.code.clear();
            out.code.push_back({Opc::Trap, 0, static_cast<std::uint16_t>(ir::TrapCode::Unreachable), 0, 0});
            out.lines.assign(1, 0);
        }
        return out;
    }

private:
    void analyze() {
        defs_.assign(fn_.regs.size(), 0);
        uses_.assign(fn_.regs.size(), 0);
        hoisted_.assign(fn_.regs.size(), false);
        for (const Inst& in : fn_.code) {
            if (in.op == Op::ScopeExit) {
                // Clearing counts as a write, so nothing outside the scope may alias these registers.
                for (Reg r = in.a; r < in.b; ++r) {
                    if (prog_.module->types.hasRefs(fn_.regs[r])) {
                        ++defs_[r];
                    }
                }
                continue;
            }
            if (in.dst != ir::kNoReg) {
                ++defs_[in.dst];
            }
            if (in.op == Op::FieldSet) {
                ++defs_[in.a];
            } else if (in.a != ir::kNoReg) {
                ++uses_[in.a];
            }
            if (in.b != ir::kNoReg && in.op != Op::EnumField) {
                ++uses_[in.b];
            }
            for (std::uint32_t i = 0; i < in.argCount; ++i) {
                ++uses_[fn_.args[in.argBegin + i]];
            }
        }
        if (!fuse) {
            return;
        }
        // Constants that are written once move to the prologue so loops do not reload them.
        for (const Inst& in : fn_.code) {
            if (in.op == Op::Const && defs_[in.dst] == 1) {
                line_ = in.line;
                emitConst(in);
                hoisted_[in.dst] = true;
            }
        }
    }

    static bool canRetarget(const Inst& in) {
        switch (in.op) {
        case Op::Const:
        case Op::Move:
        case Op::FieldGet:
        case Op::Make:
        case Op::EnumMake:
        case Op::Wrap:
        case Op::Unwrap:
        case Op::EnumField: return false;
        default: return true;
        }
    }

    void emitConst(const Inst& in) {
        const ir::Constant& c = prog_.module->constants[in.imm];
        auto v = static_cast<std::int64_t>(c.bits);
        if (c.kind == ir::ConstKind::Int && v >= INT32_MIN && v <= INT32_MAX) {
            std::size_t at = put(Opc::LoadI, s(in.dst));
            code_[at].setBc(static_cast<std::int32_t>(v));
        } else {
            std::size_t at = put(Opc::LoadK, s(in.dst));
            code_[at].setBc(static_cast<std::int32_t>(in.imm));
        }
    }

    bool aliasable(Reg dst, Reg src) const {
        return fuse && defs_[dst] == 1 && defs_[src] <= 1 && n(dst) <= n(src);
    }

    std::uint16_t s(Reg r) { return slot(offset_[r]); }

    std::uint16_t slot(std::uint32_t v) {
        if (v > 0xFFFF) {
            tooBig_ = true;
            return 0;
        }
        return static_cast<std::uint16_t>(v);
    }

    std::uint32_t n(Reg r) const { return lay_.slots(fn_.regs[r]); }
    TypeId ty(Reg r) const { return fn_.regs[r]; }

    std::size_t put(Opc op, std::uint32_t a = 0, std::uint32_t b = 0, std::uint32_t c = 0, std::uint8_t pad = 0) {
        Ins in{op, pad, slot(a), slot(b), slot(c)};
        code_.push_back(in);
        lines_.push_back(line_);
        return code_.size() - 1;
    }

    void word(std::uint32_t v) {
        Ins in{Opc::Nop, 0, 0, 0, 0};
        in.setBc(static_cast<std::int32_t>(v));
        code_.push_back(in);
        lines_.push_back(line_);
    }

    void move(std::uint32_t dst, std::uint32_t src, std::uint32_t count) {
        if (count == 0 || dst == src) {
            return;
        }
        if (count == 1) {
            put(Opc::Mov, dst, src);
        } else {
            put(Opc::MovN, dst, src, count);
        }
    }

    void patchTo(std::size_t at, std::size_t target) {
        std::int32_t off = static_cast<std::int32_t>(target) - static_cast<std::int32_t>(at) - 1;
        if (isShortJump(code_[at].op)) {
            if (off < INT16_MIN || off > INT16_MAX) {
                tooFar_ = true;
                return;
            }
            code_[at].c = static_cast<std::uint16_t>(static_cast<std::int16_t>(off));
            return;
        }
        code_[at].setBc(off);
    }

    std::uint32_t outgoing(const std::vector<Reg>& args) {
        std::uint32_t at = regSlots_;
        for (Reg r : args) {
            move(at, offset_[r], n(r));
            at += n(r);
        }
        maxOutgoing_ = std::max(maxOutgoing_, at - regSlots_);
        return regSlots_;
    }

    std::vector<Reg> argsOf(const Inst& in) const {
        return {fn_.args.begin() + in.argBegin, fn_.args.begin() + in.argBegin + in.argCount};
    }

    void normalize(TypeId t, std::uint32_t at) {
        switch (t) {
        case ir::kI8: put(Opc::Sext8, at, at); break;
        case ir::kI16: put(Opc::Sext16, at, at); break;
        case ir::kI32: put(Opc::Sext32, at, at); break;
        case ir::kU8: put(Opc::Zext8, at, at); break;
        case ir::kU16: put(Opc::Zext16, at, at); break;
        case ir::kU32: put(Opc::Zext32, at, at); break;
        default: break;
        }
    }

    void binary(const Inst& in) {
        TypeId t = ty(in.a);
        auto d = s(in.dst);
        auto a = s(in.a);
        auto b = s(in.b);
        auto shiftMask = static_cast<std::uint8_t>(prog_.module->types.bitWidth(t) - 1);
        if (t == ir::kF64 || t == ir::kF32) {
            bool f32 = t == ir::kF32;
            Opc op = Opc::Nop;
            switch (in.op) {
            case Op::Add: op = f32 ? Opc::AddF32 : Opc::AddF; break;
            case Op::Sub: op = f32 ? Opc::SubF32 : Opc::SubF; break;
            case Op::Mul: op = f32 ? Opc::MulF32 : Opc::MulF; break;
            case Op::Div: op = f32 ? Opc::DivF32 : Opc::DivF; break;
            case Op::Rem: op = f32 ? Opc::RemF32 : Opc::RemF; break;
            default: break;
            }
            put(op, d, a, b);
            return;
        }
        bool sgn = prog_.module->types.isSigned(t);
        Opc op = Opc::Nop;
        switch (in.op) {
        case Op::Add: op = Opc::AddI; break;
        case Op::Sub: op = Opc::SubI; break;
        case Op::Mul: op = Opc::MulI; break;
        case Op::Div: op = sgn ? Opc::DivI : Opc::DivU; break;
        case Op::Rem: op = sgn ? Opc::RemI : Opc::RemU; break;
        case Op::BitAnd: op = Opc::AndI; break;
        case Op::BitOr: op = Opc::OrI; break;
        case Op::BitXor: op = Opc::XorI; break;
        case Op::Shl: op = Opc::ShlI; break;
        case Op::Shr: op = sgn ? Opc::ShrI : Opc::ShrU; break;
        default: break;
        }
        put(op, d, a, b, op == Opc::ShlI || op == Opc::ShrI || op == Opc::ShrU ? shiftMask : 0);
        if (t != ir::kBool) {
            normalize(t, d);
        }
    }

    bool targetsLoop(std::uint32_t depth) const { return frames_[frames_.size() - 1 - depth].op == Op::Loop; }

    // `t = a cmp b` feeding a forward branch becomes one compare-and-jump instruction.
    bool fuseCompare(const Inst& in) {
        if (!fuse || uses_[in.dst] != 1 || pc_ + 1 >= fn_.code.size()) {
            return false;
        }
        const ir::TypeTable& types = prog_.module->types;
        TypeId t = ty(in.a);
        bool isFloat = t == ir::kF64;
        bool isInt = (types.isInteger(t) || t == ir::kBool || t == ir::kChar);
        if (!isFloat && !isInt) {
            return false;
        }
        bool uns = types.isInteger(t) && !types.isSigned(t);
        const Inst& next = fn_.code[pc_ + 1];
        bool jumpIfTrue = false;
        std::uint32_t depth = 0;
        std::size_t consumed = 0;
        if (next.op == Op::BrIf && next.a == in.dst) {
            jumpIfTrue = true;
            depth = next.imm;
            consumed = 1;
        } else if (next.op == Op::Not && next.a == in.dst && uses_[next.dst] == 1 && pc_ + 2 < fn_.code.size() &&
                   fn_.code[pc_ + 2].op == Op::BrIf && fn_.code[pc_ + 2].a == next.dst) {
            depth = fn_.code[pc_ + 2].imm;
            consumed = 2;
        } else {
            return false;
        }
        // Canonical form: x OP y with OP in {Lt, Le, Eq, Ne}.
        Op op = in.op;
        Reg x = in.a;
        Reg y = in.b;
        if (op == Op::Gt || op == Op::Ge) {
            std::swap(x, y);
            op = op == Op::Gt ? Op::Lt : Op::Le;
        }
        Opc jop = Opc::Nop;
        if (isFloat) {
            if (jumpIfTrue) {
                jop = op == Op::Lt ? Opc::JLtF : op == Op::Le ? Opc::JLeF : op == Op::Eq ? Opc::JEqF : Opc::JNeF;
            } else {
                jop = op == Op::Lt ? Opc::JNLtF : op == Op::Le ? Opc::JNLeF : op == Op::Eq ? Opc::JNeF : Opc::JEqF;
            }
        } else {
            if (!jumpIfTrue) {
                if (op == Op::Lt || op == Op::Le) {
                    std::swap(x, y);
                    op = op == Op::Lt ? Op::Le : Op::Lt;
                } else {
                    op = op == Op::Eq ? Op::Ne : Op::Eq;
                }
            }
            jop = op == Op::Eq   ? Opc::JEqI
                  : op == Op::Ne ? Opc::JNeI
                  : op == Op::Lt ? (uns ? Opc::JLtU : Opc::JLtI)
                                 : (uns ? Opc::JLeU : Opc::JLeI);
        }
        branchTo(depth, put(jop, s(x), s(y)));
        skip_ = consumed;
        return true;
    }

    void branchTo(std::uint32_t depth, std::size_t at) {
        Frame& f = frames_[frames_.size() - 1 - depth];
        if (f.op == Op::Loop) {
            patchTo(at, f.start);
        } else {
            f.exits.push_back(at);
        }
    }

    [[nodiscard]] bool isConstOne(Reg r) const {
        if (!hoisted_[r]) {
            return false;
        }
        for (const Inst& in : fn_.code) {
            if (in.op == Op::Const && in.dst == r) {
                const ir::Constant& c = prog_.module->constants[in.imm];
                return c.kind == ir::ConstKind::Int && c.bits == 1;
            }
        }
        return false;
    }

    // Loop tails produced by the rotated lowering become one instruction (like Lua's FORLOOP).
    bool fuseLoopTail(const Inst& in) {
        if (!fuse || pc_ + 2 >= fn_.code.size()) {
            return false;
        }
        const std::vector<Inst>& code = fn_.code;
        TypeId t = ty(in.dst);
        // i = i + 1; t = i < hi; br_if t $loop
        if (in.op == Op::Add && in.dst == in.a && isConstOne(in.b) && t == ir::kI64) {
            const Inst& cmp = code[pc_ + 1];
            const Inst& br = code[pc_ + 2];
            if (cmp.op == Op::Lt && cmp.a == in.dst && uses_[cmp.dst] == 1 && br.op == Op::BrIf && br.a == cmp.dst &&
                targetsLoop(br.imm)) {
                branchTo(br.imm, put(Opc::ForI, s(in.dst), s(cmp.b)));
                skip_ = 2;
                return true;
            }
            // i = i + 1; len = list.len(); t = i < len; br_if t $loop
            if (pc_ + 3 < code.size() && cmp.op == Op::Intrinsic &&
                static_cast<Intrinsic>(cmp.imm) == Intrinsic::ListLen && uses_[cmp.dst] == 1) {
                const Inst& lt = code[pc_ + 2];
                const Inst& br2 = code[pc_ + 3];
                if (lt.op == Op::Lt && lt.a == in.dst && lt.b == cmp.dst && uses_[lt.dst] == 1 && br2.op == Op::BrIf &&
                    br2.a == lt.dst && targetsLoop(br2.imm)) {
                    branchTo(br2.imm, put(Opc::ForList, s(in.dst), s(fn_.args[cmp.argBegin])));
                    skip_ = 3;
                    return true;
                }
            }
        }
        return false;
    }

    bool fuseInclusiveTail(const Inst& in) {
        if (!fuse || pc_ + 4 >= fn_.code.size() || ty(in.a) != ir::kI64) {
            return false;
        }
        const std::vector<Inst>& code = fn_.code;
        const Inst& no = code[pc_ + 1];
        const Inst& exitBr = code[pc_ + 2];
        const Inst& add = code[pc_ + 3];
        const Inst& back = code[pc_ + 4];
        if (in.op == Op::Lt && uses_[in.dst] == 1 && no.op == Op::Not && no.a == in.dst && uses_[no.dst] == 1 &&
            exitBr.op == Op::BrIf && exitBr.a == no.dst && !targetsLoop(exitBr.imm) && add.op == Op::Add &&
            add.dst == in.a && add.a == in.a && isConstOne(add.b) && back.op == Op::Br && targetsLoop(back.imm)) {
            // The exit target must be the block right around the loop, which `ForIncl` falls through to.
            if (exitBr.imm != back.imm + 1) {
                return false;
            }
            branchTo(back.imm, put(Opc::ForIncl, s(in.a), s(in.b)));
            skip_ = 4;
            return true;
        }
        return false;
    }

    void compare(const Inst& in) {
        if (fuseCompare(in)) {
            return;
        }
        TypeId t = ty(in.a);
        const ir::TypeTable& types = prog_.module->types;
        auto d = s(in.dst);
        auto a = s(in.a);
        auto b = s(in.b);
        bool swap = in.op == Op::Gt || in.op == Op::Ge;
        bool lt = in.op == Op::Lt || in.op == Op::Gt;
        bool le = in.op == Op::Le || in.op == Op::Ge;
        bool eq = in.op == Op::Eq;
        auto emitCmp = [&](Opc eqOp, Opc neOp, Opc ltOp, Opc leOp) {
            Opc op = eq ? eqOp : lt ? ltOp : le ? leOp : neOp;
            if (swap) {
                put(op, d, b, a);
            } else {
                put(op, d, a, b);
            }
        };
        if (t == ir::kF64) {
            emitCmp(Opc::EqF, Opc::NeF, Opc::LtF, Opc::LeF);
        } else if (t == ir::kF32) {
            emitCmp(Opc::EqF32, Opc::NeF32, Opc::LtF32, Opc::LeF32);
        } else if (t == ir::kStr || (types.kind(t) == TypeKind::Nullable && types.info(t).elem == ir::kStr)) {
            emitCmp(Opc::EqS, Opc::NeS, Opc::LtS, Opc::LeS);
        } else if (types.kind(t) == TypeKind::Nullable && !lay_.nullableIsPointer(t)) {
            TypeId inner = types.info(t).elem;
            put(Opc::EqN, d, a, b, inner == ir::kF64 ? 1 : inner == ir::kF32 ? 2 : 0);
            word(n(in.a));
            if (in.op == Op::Ne) {
                put(Opc::NotB, d, d);
            }
        } else if (types.isInteger(t) && !types.isSigned(t)) {
            emitCmp(Opc::EqI, Opc::NeI, Opc::LtU, Opc::LeU);
        } else {
            emitCmp(Opc::EqI, Opc::NeI, Opc::LtI, Opc::LeI);
        }
    }

    void convert(const Inst& in) {
        const ir::TypeTable& types = prog_.module->types;
        TypeId from = ty(in.a);
        TypeId to = ty(in.dst);
        auto d = s(in.dst);
        auto a = s(in.a);
        bool fromF = types.isFloat(from);
        bool toF = types.isFloat(to);
        if (fromF && toF) {
            put(from == to ? Opc::Mov : (to == ir::kF32 ? Opc::F2F32 : Opc::F322F), d, a);
        } else if (fromF) {
            put(from == ir::kF32 ? Opc::F322I : Opc::F2I, d, a, 0, static_cast<std::uint8_t>(to));
        } else if (toF) {
            bool uns = types.isInteger(from) && !types.isSigned(from);
            if (to == ir::kF32) {
                put(uns ? Opc::U2F32 : Opc::I2F32, d, a);
            } else {
                put(uns ? Opc::U2F : Opc::I2F, d, a);
            }
        } else {
            move(d, a, 1);
            normalize(to, d);
        }
    }

    void intrinsic(const Inst& in) {
        auto op = static_cast<Intrinsic>(in.imm);
        std::vector<Reg> a = argsOf(in);
        auto d = in.dst != ir::kNoReg ? s(in.dst) : std::uint16_t{0};
        auto sa = [&](std::size_t i) { return s(a[i]); };
        switch (op) {
        case Intrinsic::ListNew:
            put(Opc::ListNew, d), word(prog_.module->types.info(ty(in.dst)).elem);
            return;
        case Intrinsic::ListFilled:
            put(Opc::ListFilled, d, sa(0), sa(1));
            word(prog_.module->types.info(ty(in.dst)).elem);
            return;
        case Intrinsic::ListLen: put(Opc::ListLen, d, sa(0)); return;
        case Intrinsic::ListGet:
            put(ty(in.dst) == ir::kBool ? Opc::ListGetB : n(in.dst) == 1 ? Opc::ListGet : Opc::ListGetN, d, sa(0),
                sa(1));
            return;
        case Intrinsic::ListSet: {
            TypeId elem = prog_.module->types.info(ty(a[0])).elem;
            bool simple = lay_.slots(elem) == 1 && !prog_.module->types.hasRefs(elem);
            put(elem == ir::kBool ? Opc::ListSetB : simple ? Opc::ListSet : Opc::ListSetN, sa(0), sa(1), sa(2));
            return;
        }
        case Intrinsic::ListPush: put(Opc::ListPush, sa(0), sa(1)); return;
        case Intrinsic::ListPop: put(Opc::ListPop, d, sa(0)); return;
        case Intrinsic::ListClear: put(Opc::ListClear, sa(0)); return;
        case Intrinsic::ListJoin: put(Opc::ListJoin, d, sa(0), sa(1)); return;
        case Intrinsic::MapNew:
            put(Opc::MapNew, d), word(ty(in.dst));
            return;
        case Intrinsic::MapNext:
            put(Opc::MapNext, d, sa(0), sa(1));
            return;
        case Intrinsic::MapKeyAt:
        case Intrinsic::MapValueAt:
            put(Opc::MapEnt, d, sa(0), sa(1), op == Intrinsic::MapValueAt);
            return;
        case Intrinsic::StrDecode:
            put(Opc::StrDecode, d, sa(0), sa(1));
            return;
        case Intrinsic::StrFormat:
            put(Opc::StrFmt, d, sa(0), sa(1), formatKind(ty(a[0])));
            return;
        case Intrinsic::MapLen: put(Opc::MapLen, d, sa(0)); return;
        case Intrinsic::MapGet: put(Opc::MapGet, d, sa(0), sa(1)); return;
        case Intrinsic::MapSet: put(Opc::MapSet, sa(0), sa(1), sa(2)); return;
        case Intrinsic::MapHas: put(Opc::MapHas, d, sa(0), sa(1)); return;
        case Intrinsic::MapRemove: put(Opc::MapRemove, d, sa(0), sa(1)); return;
        case Intrinsic::StrLen: put(Opc::StrLen, d, sa(0)); return;
        case Intrinsic::StrConcat: {
            std::uint32_t base = outgoing(a);
            put(Opc::StrConcat, d, base, static_cast<std::uint32_t>(a.size()));
            return;
        }
        case Intrinsic::StrSlice: {
            std::uint32_t base = outgoing({a[1], a[2]});
            put(Opc::StrSlice, d, sa(0), base);
            return;
        }
        case Intrinsic::StrLower: put(Opc::StrLower, d, sa(0)); return;
        case Intrinsic::StrUpper: put(Opc::StrUpper, d, sa(0)); return;
        case Intrinsic::StrContains: put(Opc::StrContains, d, sa(0), sa(1)); return;
        case Intrinsic::StrFind: put(Opc::StrFind, d, sa(0), sa(1)); return;
        case Intrinsic::StrStartsWith: put(Opc::StrStarts, d, sa(0), sa(1)); return;
        case Intrinsic::StrEndsWith: put(Opc::StrEnds, d, sa(0), sa(1)); return;
        case Intrinsic::StrByteAt: put(Opc::StrByteAt, d, sa(0), sa(1)); return;
        case Intrinsic::StrFromI64: put(Opc::StrFromI, d, sa(0)); return;
        case Intrinsic::StrFromU64: put(Opc::StrFromU, d, sa(0)); return;
        case Intrinsic::StrFromF64: put(Opc::StrFromF, d, sa(0)); return;
        case Intrinsic::StrFromBool: put(Opc::StrFromB, d, sa(0)); return;
        case Intrinsic::StrFromChar: put(Opc::StrFromC, d, sa(0)); return;
        case Intrinsic::MathSqrt: put(Opc::Sqrt, d, sa(0)); return;
        case Intrinsic::MathFloor: put(Opc::Floor, d, sa(0)); return;
        case Intrinsic::MathCeil: put(Opc::Ceil, d, sa(0)); return;
        case Intrinsic::MathAbsF: put(Opc::AbsF, d, sa(0)); return;
        case Intrinsic::MathAbsI: put(Opc::AbsI, d, sa(0)); return;
        case Intrinsic::MathMinI: put(Opc::MinI, d, sa(0), sa(1)); return;
        case Intrinsic::MathMaxI: put(Opc::MaxI, d, sa(0), sa(1)); return;
        case Intrinsic::MathMinF: put(Opc::MinF, d, sa(0), sa(1)); return;
        case Intrinsic::MathMaxF: put(Opc::MaxF, d, sa(0), sa(1)); return;
        case Intrinsic::MathPow: put(Opc::Pow, d, sa(0), sa(1)); return;
        case Intrinsic::MathSin: put(Opc::Sin, d, sa(0)); return;
        case Intrinsic::MathCos: put(Opc::Cos, d, sa(0)); return;
        case Intrinsic::Panic: put(Opc::Panic, sa(0)); return;
        case Intrinsic::Count: return;
        default:
            extraIntrinsic(op, a, d);
            return;
        }
    }

    // Intrinsics added after Panic; runtime builtins read their arguments from the outgoing area.
    void extraIntrinsic(Intrinsic op, const std::vector<Reg>& a, std::uint16_t d) {
        if (ir::isRuntimeBuiltin(op)) {
            std::uint32_t base = outgoing(a);
            put(Opc::Builtin, d, base, ir::runtimeBuiltinIndex(op));
            return;
        }
        auto sa = [&](std::size_t i) { return s(a[i]); };
        switch (op) {
        case Intrinsic::CharCode:
            move(d, sa(0), 1);
            return;
        case Intrinsic::MathAtan2:
            put(Opc::Atan2, d, sa(0), sa(1));
            return;
        case Intrinsic::MathFmod:
            put(Opc::RemF, d, sa(0), sa(1));
            return;
        case Intrinsic::MathClampI:
        case Intrinsic::MathClampF: {
            bool f = op == Intrinsic::MathClampF;
            std::uint32_t t = outgoing({a[0]});
            put(f ? Opc::MaxF : Opc::MaxI, t, t, sa(1));
            put(f ? Opc::MinF : Opc::MinI, d, t, sa(2));
            return;
        }
        default:
            put(Opc::MathF1, d, sa(0), static_cast<std::uint32_t>(op) - static_cast<std::uint32_t>(Intrinsic::MathTan));
            return;
        }
    }

    struct Frame {
        Op op;
        std::size_t start;
        std::vector<std::size_t> exits;
        std::size_t elseJump = ~std::size_t{0};
    };

    void lower(const Inst& in) {
        const ir::TypeTable& types = prog_.module->types;
        if (in.op == Op::Add && fuseLoopTail(in)) {
            return;
        }
        if (in.op == Op::Lt && fuseInclusiveTail(in)) {
            return;
        }
        switch (in.op) {
        case Op::Block: frames_.push_back({Op::Block, code_.size(), {}, ~std::size_t{0}}); return;
        case Op::Loop:
            loopLines_.emplace_back(static_cast<std::uint32_t>(pc_), lines_.empty() ? 0 : lines_.back());
            frames_.push_back({Op::Loop, code_.size(), {}, ~std::size_t{0}});
            return;
        case Op::If: {
            std::size_t j = put(Opc::JmpF, s(in.a));
            frames_.push_back({Op::If, code_.size(), {}, j});
            return;
        }
        case Op::Else: {
            Frame& f = frames_.back();
            f.exits.push_back(put(Opc::Jmp));
            patchTo(f.elseJump, code_.size());
            f.elseJump = ~std::size_t{0};
            return;
        }
        case Op::End: {
            Frame f = std::move(frames_.back());
            frames_.pop_back();
            if (f.elseJump != ~std::size_t{0}) {
                patchTo(f.elseJump, code_.size());
            }
            for (std::size_t e : f.exits) {
                patchTo(e, code_.size());
            }
            return;
        }
        case Op::Br:
        case Op::BrIf: {
            Frame& f = frames_[frames_.size() - 1 - in.imm];
            bool cond = in.op == Op::BrIf;
            if (f.op == Op::Loop) {
                std::size_t j = cond ? put(Opc::LoopT, s(in.a)) : put(Opc::Loop);
                patchTo(j, f.start);
            } else {
                f.exits.push_back(cond ? put(Opc::JmpT, s(in.a)) : put(Opc::Jmp));
            }
            return;
        }
        case Op::Return:
            if (in.a == ir::kNoReg) {
                put(Opc::Ret0);
            } else {
                put(Opc::Ret, s(in.a), n(in.a));
            }
            return;
        case Op::Trap: put(Opc::Trap, in.imm); return;
        case Op::Const:
            if (!hoisted_[in.dst]) {
                emitConst(in);
            }
            return;
        case Op::Move:
            if (aliasable(in.dst, in.a)) {
                offset_[in.dst] = offset_[in.a];
                return;
            }
            move(s(in.dst), s(in.a), n(in.a));
            return;
        case Op::Not:
            if (fuse && pc_ + 1 < fn_.code.size() && uses_[in.dst] == 1 && fn_.code[pc_ + 1].op == Op::BrIf &&
                fn_.code[pc_ + 1].a == in.dst && !targetsLoop(fn_.code[pc_ + 1].imm)) {
                frames_[frames_.size() - 1 - fn_.code[pc_ + 1].imm].exits.push_back(put(Opc::JmpF, s(in.a)));
                skip_ = 1;
                return;
            }
            put(Opc::NotB, s(in.dst), s(in.a));
            return;
        case Op::Neg: {
            TypeId t = ty(in.a);
            put(t == ir::kF64 ? Opc::NegF : t == ir::kF32 ? Opc::NegF32 : Opc::NegI, s(in.dst), s(in.a));
            if (types.isInteger(t)) {
                normalize(t, s(in.dst));
            }
            return;
        }
        case Op::BitNot:
            put(Opc::BNotI, s(in.dst), s(in.a));
            normalize(ty(in.a), s(in.dst));
            return;
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Rem:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr: binary(in); return;
        case Op::Eq:
        case Op::Ne:
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge: compare(in); return;
        case Op::Convert: convert(in); return;
        case Op::Call:
        case Op::CallHost: {
            std::uint32_t base = outgoing(argsOf(in));
            std::uint32_t dst = in.dst != ir::kNoReg ? offset_[in.dst] : 0;
            put(in.op == Op::Call ? Opc::Call : Opc::CallHost, dst, in.imm, base);
            if (in.op == Op::Call && prog_.module->functions[in.imm].isSuspend) {
                recordResume();
            }
            return;
        }
        case Op::CallValue: {
            std::uint32_t base = outgoing(argsOf(in));
            put(Opc::CallV, in.dst != ir::kNoReg ? offset_[in.dst] : 0, s(in.a), base);
            return;
        }
        case Op::Closure: {
            std::vector<Reg> caps = argsOf(in);
            if (caps.empty()) {
                std::size_t at = put(Opc::LoadK, s(in.dst));
                code_[at].setBc(static_cast<std::int32_t>(prog_.closureConst[in.imm]));
                return;
            }
            std::uint32_t slots = 0;
            for (Reg r : caps) {
                slots += n(r);
            }
            std::uint32_t base = outgoing(caps);
            put(Opc::Clo, s(in.dst), 0, base);
            word(in.imm);
            word(static_cast<std::uint32_t>(caps.size()) << 16 | slots);
            return;
        }
        case Op::Intrinsic: intrinsic(in); return;
        case Op::FieldGet: {
            TypeId agg = ty(in.a);
            std::uint32_t off = lay_.fieldOffset(agg, in.imm);
            if (types.kind(agg) == TypeKind::State) {
                if (n(in.dst) == 1) {
                    put(Opc::SGet, s(in.dst), s(in.a), off);
                } else {
                    put(Opc::SGetN, s(in.dst), s(in.a), off);
                    word(n(in.dst));
                }
            } else if (aliasable(in.dst, in.a)) {
                offset_[in.dst] = offset_[in.a] + off;
            } else {
                move(s(in.dst), offset_[in.a] + off, n(in.dst));
            }
            return;
        }
        case Op::FieldSet: {
            TypeId agg = ty(in.a);
            std::uint32_t off = lay_.fieldOffset(agg, in.imm);
            if (types.kind(agg) == TypeKind::State) {
                if (n(in.b) == 1 && !types.hasRefs(ty(in.b))) {
                    put(Opc::SSet, s(in.a), s(in.b), off);
                } else {
                    put(Opc::SSetR, s(in.a), s(in.b), off);
                    word(ty(in.b));
                }
            } else {
                move(offset_[in.a] + off, s(in.b), n(in.b));
            }
            return;
        }
        case Op::Make: {
            std::vector<Reg> a = argsOf(in);
            for (std::size_t i = 0; i < a.size(); ++i) {
                move(offset_[in.dst] + lay_.fieldOffset(ty(in.dst), static_cast<std::uint32_t>(i)), s(a[i]), n(a[i]));
            }
            return;
        }
        case Op::IsNull:
            put(lay_.nullableIsPointer(ty(in.a)) ? Opc::IsNullP : Opc::IsNullF, s(in.dst), s(in.a));
            return;
        case Op::Unwrap:
            if (lay_.nullableIsPointer(ty(in.a))) {
                put(Opc::UnwrapP, s(in.dst), s(in.a));
            } else {
                put(Opc::UnwrapF, s(in.dst), s(in.a), n(in.dst));
            }
            return;
        case Op::Wrap:
            if (lay_.nullableIsPointer(ty(in.dst))) {
                move(s(in.dst), s(in.a), 1);
            } else {
                put(Opc::WrapF, s(in.dst), s(in.a), n(in.a));
            }
            return;
        case Op::NullOf: put(lay_.nullableIsPointer(ty(in.dst)) ? Opc::NullP : Opc::NullF, s(in.dst)); return;
        case Op::EnumMake: {
            std::size_t at = put(Opc::LoadI, s(in.dst));
            code_[at].setBc(static_cast<std::int32_t>(in.imm));
            std::vector<Reg> a = argsOf(in);
            std::uint32_t off = offset_[in.dst] + 1;
            for (Reg r : a) {
                move(off, s(r), n(r));
                off += n(r);
            }
            return;
        }
        case Op::EnumTag: move(s(in.dst), s(in.a), 1); return;
        case Op::ScopeEnter: put(Opc::ScopeIn); return;
        case Op::ScopeExit: {
            put(Opc::ScopeOut);
            // Registers written inside the scope may point into the region just freed.
            for (Reg r = in.a; r < in.b; ++r) {
                if (types.hasRefs(ty(r)) && !hoisted_[r] && offset_[r] == origOffset_[r]) {
                    put(Opc::Zero, offset_[r], 0, n(r));
                }
            }
            return;
        }
        case Op::Persist:
            put(Opc::Persist, s(in.a), 0, in.imm);
            word(ty(in.a));
            return;
        case Op::Wait:
            put(Opc::Wait, s(in.a), in.dst == ir::kNoReg ? 0xFFFF : s(in.dst), in.imm);
            word(in.dst == ir::kNoReg ? ir::kVoid : ty(in.dst));
            recordResume();
            return;
        case Op::EnumField: {
            const ir::VariantInfo& v = types.info(ty(in.a)).variants[in.imm];
            std::uint32_t off = offset_[in.a] + 1;
            for (std::uint32_t i = 0; i < in.b; ++i) {
                off += lay_.slots(v.payload[i]);
            }
            // The validator cannot prove the tag, and another variant's slots would be read as this payload's type.
            std::size_t load = put(Opc::LoadI, regSlots_);
            code_[load].setBc(static_cast<std::int32_t>(in.imm));
            put(Opc::JEqI, s(in.a), regSlots_, 1);
            put(Opc::Trap, static_cast<std::uint32_t>(ir::TrapCode::Unreachable));
            maxOutgoing_ = std::max<std::uint32_t>(maxOutgoing_, 1);
            move(s(in.dst), off, n(in.dst));
            return;
        }
        }
    }

    Program& prog_;
    const ir::Function& fn_;
    const Layouts& lay_;
    std::vector<std::uint32_t> offset_;
    std::uint32_t regSlots_ = 0;
    std::uint32_t maxOutgoing_ = 0;
    std::vector<Ins> code_;
    std::vector<std::uint32_t> lines_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> loopLines_;
    std::unique_ptr<ir::Liveness> live_;
    std::vector<CompiledFunction::ResumeRecord> resumeRecords_;

    // Where each register kept by the native frame record lives in this interpreter frame (D69).
    void recordResume() {
        CompiledFunction::ResumeRecord r;
        r.resumePc = static_cast<std::uint32_t>(code_.size());
        r.state = ir::resumeState(pc_, ir::ResumeAt::After);
        for (Reg k : live_->kept(pc_, ir::ResumeAt::After)) {
            r.slots.emplace_back(offset_[k], n(k));
        }
        resumeRecords_.push_back(std::move(r));
    }
    std::vector<Frame> frames_;
    std::size_t pc_ = 0;
    std::uint32_t line_ = 0;
    bool tooBig_ = false;
    bool tooFar_ = false;
    std::vector<std::uint32_t> origOffset_;
    std::vector<std::uint32_t> defs_;
    std::vector<std::uint32_t> uses_;
    std::vector<bool> hoisted_;
    std::size_t skip_ = 0;
    Reg retarget_ = ir::kNoReg; // destination override for the instruction being lowered
    Reg retargetFrom_ = ir::kNoReg;

public:
    bool fuse = true;
    [[nodiscard]] bool needsRelax() const { return tooFar_; }
};

} // namespace

std::unique_ptr<Program> compile(std::unique_ptr<ir::Module> module) {
    auto prog = std::make_unique<Program>();
    prog->module = std::move(module);
    prog->layouts = std::make_unique<Layouts>(prog->module->types);
    for (const ir::Constant& c : prog->module->constants) {
        Slot v{};
        if (c.kind == ir::ConstKind::Str) {
            std::size_t size = kStrHeader + c.text.size() + 1;
            auto mem = std::make_unique<std::byte[]>(size);
            auto* str = reinterpret_cast<Str*>(mem.get());
            str->len = static_cast<std::uint32_t>(c.text.size());
            str->hash = 0;
            str->owner = Owner::Static;
            std::memcpy(str->data, c.text.data(), c.text.size());
            str->data[c.text.size()] = '\0';
            strHash(str);
            v.p = str;
            prog->staticStrings.push_back(std::move(mem));
        } else if (c.kind == ir::ConstKind::Float) {
            double d = std::bit_cast<double>(c.bits);
            if (c.type == ir::kF32) {
                v.u = 0;
                v.f32 = static_cast<float>(d);
            } else {
                v.f = d;
            }
        } else {
            v.u = c.bits;
        }
        prog->constants.push_back(v);
    }
    prog->closureConst.assign(prog->module->functions.size(), ~0U);
    for (const ir::Function& fn : prog->module->functions) {
        for (const ir::Inst& in : fn.code) {
            if (in.op != ir::Op::Closure || in.argCount != 0 || prog->closureConst[in.imm] != ~0U) {
                continue;
            }
            auto mem = std::make_unique<std::byte[]>(sizeof(Closure));
            auto* c = reinterpret_cast<Closure*>(mem.get());
            *c = Closure{in.imm, 0, 0, Owner::Static, 0, {}, {}};
            Slot v{};
            v.p = c;
            prog->closureConst[in.imm] = static_cast<std::uint32_t>(prog->constants.size());
            prog->constants.push_back(v);
            prog->staticStrings.push_back(std::move(mem));
        }
    }
    for (const ir::HostImport& imp : prog->module->imports) {
        prog->importResultSlots.push_back(prog->layouts->slots(imp.result));
        bool c = prog->module->types.info(imp.result).reprC;
        for (ir::TypeId p : imp.params) {
            c = c || prog->module->types.info(p).reprC;
        }
        prog->importReprC.push_back(c);
    }
    for (const ir::Function& fn : prog->module->functions) {
        FunctionCompiler fc(*prog, fn);
        CompiledFunction cf = fc.run();
        if (fc.needsRelax()) {
            FunctionCompiler plain(*prog, fn);
            plain.fuse = false;
            cf = plain.run();
        }
        for (TypeId p : cf.params) {
            std::int32_t plan = prog->layouts->slots(p) >= 1 ? CompiledFunction::kHostArg : CompiledFunction::kBadArg;
            std::int32_t handleArg = -1;
            for (std::size_t s = 0; s < prog->module->states.size(); ++s) {
                if (prog->module->states[s] == p) {
                    plan = static_cast<std::int32_t>(s);
                    TypeId per = prog->module->types.info(p).persistPer;
                    std::int32_t hostIndex = 0;
                    for (TypeId q : cf.params) {
                        bool isState = prog->module->types.kind(q) == ir::TypeKind::State;
                        if (per != 0 && q == per && handleArg < 0) {
                            handleArg = hostIndex;
                        }
                        hostIndex += isState ? 0 : static_cast<std::int32_t>(prog->layouts->slots(q));
                    }
                    if (per != 0 && handleArg < 0) {
                        plan = CompiledFunction::kBadArg; // the validator-free path: no handle to pick the instance
                    }
                }
            }
            cf.entryPlan.push_back(plan);
            cf.entryHandle.push_back(handleArg);
        }
        prog->functions.push_back(std::move(cf));
        prog->maxFrameSlots = std::max(prog->maxFrameSlots, prog->functions.back().frameSlots);
    }
    prog->tier = makeTierState(prog->functions.size());
    return prog;
}

std::string disassemble(const Program& p, std::uint32_t function) {
    const CompiledFunction& f = p.functions[function];
    std::string out = std::format("{} (params {} slots, frame {} slots)\n", f.name, f.paramSlots, f.frameSlots);
    for (std::size_t i = 0; i < f.code.size(); ++i) {
        const Ins& in = f.code[i];
        out += std::format("  {:4} {:<11} {} {} {}\n", i, opcodeName(in.op), in.a, in.b, in.c);
    }
    return out;
}

} // namespace quartz::vm
