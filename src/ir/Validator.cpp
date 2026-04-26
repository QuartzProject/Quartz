#include "ir/Validator.hpp"

#include "ir/Builtins.hpp"

#include <algorithm>
#include <format>
#include <span>

namespace quartz::ir {

std::string ValidationError::describe(const Module& m) const {
    if (function < m.functions.size()) {
        return std::format("invalid IR in '{}' at instruction {}: {}", m.functions[function].name, inst, message);
    }
    return std::format("invalid IR: {}", message);
}

namespace {

struct InitState {
    bool unreachable = false;
    std::uint32_t scopes = 0; // open `scope` regions on this path
    std::vector<bool> init;

    /// Returns false when two reachable paths disagree on the open scopes.
    bool meet(const InitState& other) {
        if (other.unreachable) {
            return true;
        }
        if (unreachable) {
            *this = other;
            return true;
        }
        for (std::size_t i = 0; i < init.size(); ++i) {
            init[i] = init[i] && other.init[i];
        }
        return scopes == other.scopes;
    }
};

struct Frame {
    Op op;
    InitState entry;
    InitState exits; // meet of every branch out of the frame; starts unreachable
    InitState thenEnd;
    bool sawElse = false;
};

class FunctionValidator {
public:
    FunctionValidator(const Module& m, std::uint32_t index) : m_(m), t_(m.types), fn_(m.functions[index]), index_(index) {}

    std::expected<void, ValidationError> run() {
        if (fn_.paramCount > fn_.regs.size()) {
            return fail("more parameters than registers");
        }
        if (!validValueType(fn_.result, true)) {
            return fail("invalid result type");
        }
        for (TypeId r : fn_.regs) {
            if (!validValueType(r, false)) {
                return fail(std::format("register of invalid type {}", r));
            }
        }
        for (std::uint32_t init : m_.stateInit) {
            if (init == index_ && fn_.paramCount == 1 && t_.kind(fn_.regs[0]) == TypeKind::State) {
                stateFields_ = static_cast<std::uint32_t>(t_.info(fn_.regs[0]).fields.size());
            }
        }
        state_.init.assign(fn_.regs.size() + stateFields_, false);
        for (std::uint32_t p = 0; p < fn_.paramCount; ++p) {
            state_.init[p] = true;
        }
        for (pc_ = 0; pc_ < fn_.code.size(); ++pc_) {
            if (auto r = step(fn_.code[pc_]); !r) {
                return r;
            }
        }
        if (!frames_.empty()) {
            return fail("unterminated block");
        }
        if (!state_.unreachable) {
            return fail("function can fall off its end; it must end with return or trap");
        }
        return {};
    }

private:
    std::unexpected<ValidationError> fail(std::string msg) const {
        return std::unexpected(ValidationError{index_, static_cast<std::uint32_t>(pc_), std::move(msg)});
    }

    [[nodiscard]] bool validType(TypeId t) const { return t < t_.size(); }

    [[nodiscard]] bool validValueType(TypeId t, bool allowVoid) const {
        if (!validType(t)) {
            return false;
        }
        if (t == kVoid) {
            return allowVoid;
        }
        return t != kError && t != kNull && t != kNever && !t_.hasParams(t) && !t_.isGenericBase(t);
    }

    bool reg(Reg r) const { return r < fn_.regs.size(); }

    std::expected<void, ValidationError> use(Reg r) {
        if (!reg(r)) {
            return fail(std::format("register r{} out of range", r));
        }
        if (!state_.unreachable && !state_.init[r]) {
            return fail(std::format("register r{} read before it is written", r));
        }
        return {};
    }

    std::expected<void, ValidationError> def(Reg r) {
        if (!reg(r)) {
            return fail(std::format("register r{} out of range", r));
        }
        if (!state_.unreachable) {
            state_.init[r] = true;
        }
        return {};
    }

    TypeId ty(Reg r) const { return fn_.regs[r]; }

    std::span<const Reg> args(const Inst& in) const {
        return std::span<const Reg>(fn_.args).subspan(in.argBegin, in.argCount);
    }

    [[nodiscard]] bool equatable(TypeId t) const {
        const TypeInfo& info = t_.info(t);
        if (t_.isScalar(t) || t == kStr || info.kind == TypeKind::Handle) {
            return true;
        }
        if (info.kind == TypeKind::Nullable) {
            TypeId e = info.elem;
            return t_.isScalar(e) || e == kStr || t_.kind(e) == TypeKind::Handle;
        }
        if (info.kind == TypeKind::Enum) {
            for (const VariantInfo& v : info.variants) {
                if (!v.payload.empty()) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] bool convertible(TypeId from, TypeId to) const {
        return from == to || (t_.isNumeric(from) && t_.isNumeric(to)) ||
               ((from == kChar || from == kBool) && t_.isInteger(to)) || (from == kU8 && to == kChar);
    }

    std::expected<void, ValidationError> intrinsic(const Inst& in) {
        if (in.imm >= static_cast<std::uint32_t>(Intrinsic::Count)) {
            return fail("unknown intrinsic");
        }
        auto op = static_cast<Intrinsic>(in.imm);
        std::span<const Reg> a = args(in);
        std::vector<TypeId> at;
        for (Reg r : a) {
            at.push_back(ty(r));
        }
        TypeId dst = in.dst == kNoReg ? kVoid : ty(in.dst);
        auto kindOf = [&](std::size_t i) { return i < at.size() ? t_.kind(at[i]) : TypeKind::Error; };
        auto sig = [&](std::initializer_list<TypeId> params, TypeId result) {
            if (at.size() != params.size() || dst != result) {
                return false;
            }
            std::size_t i = 0;
            for (TypeId p : params) {
                if (at[i++] != p) {
                    return false;
                }
            }
            return true;
        };
        bool ok = false;
        switch (op) {
        case Intrinsic::ListNew: ok = at.empty() && t_.kind(dst) == TypeKind::List; break;
        case Intrinsic::ListFilled:
            ok = t_.kind(dst) == TypeKind::List && sig({kI64, t_.info(dst).elem}, dst);
            break;
        case Intrinsic::ListLen: ok = kindOf(0) == TypeKind::List && sig({at[0]}, kI64); break;
        case Intrinsic::ListGet: ok = kindOf(0) == TypeKind::List && sig({at[0], kI64}, t_.info(at[0]).elem); break;
        case Intrinsic::ListSet:
            ok = kindOf(0) == TypeKind::List && sig({at[0], kI64, t_.info(at[0]).elem}, kVoid);
            break;
        case Intrinsic::ListPush: ok = kindOf(0) == TypeKind::List && sig({at[0], t_.info(at[0]).elem}, kVoid); break;
        case Intrinsic::ListPop:
            ok = kindOf(0) == TypeKind::List && at.size() == 1 && t_.kind(dst) == TypeKind::Nullable &&
                 t_.info(dst).elem == t_.info(at[0]).elem;
            break;
        case Intrinsic::ListClear: ok = kindOf(0) == TypeKind::List && sig({at[0]}, kVoid); break;
        case Intrinsic::ListJoin:
            ok = kindOf(0) == TypeKind::List && t_.info(at[0]).elem == kStr && sig({at[0], kStr}, kStr);
            break;
        case Intrinsic::MapNew: ok = at.empty() && t_.kind(dst) == TypeKind::Map; break;
        case Intrinsic::MapLen: ok = kindOf(0) == TypeKind::Map && sig({at[0]}, kI64); break;
        case Intrinsic::MapGet:
            ok = kindOf(0) == TypeKind::Map && at.size() == 2 && at[1] == t_.info(at[0]).key &&
                 t_.kind(dst) == TypeKind::Nullable && t_.info(dst).elem == t_.info(at[0]).elem;
            break;
        case Intrinsic::MapSet:
            ok = kindOf(0) == TypeKind::Map && sig({at[0], t_.info(at[0]).key, t_.info(at[0]).elem}, kVoid);
            break;
        case Intrinsic::MapHas:
        case Intrinsic::MapRemove: ok = kindOf(0) == TypeKind::Map && sig({at[0], t_.info(at[0]).key}, kBool); break;
        case Intrinsic::StrLen: ok = sig({kStr}, kI64); break;
        case Intrinsic::StrConcat: {
            ok = !at.empty() && dst == kStr;
            for (TypeId x : at) {
                ok = ok && x == kStr;
            }
            break;
        }
        case Intrinsic::StrSlice: ok = sig({kStr, kI64, kI64}, kStr); break;
        case Intrinsic::StrLower:
        case Intrinsic::StrUpper: ok = sig({kStr}, kStr); break;
        case Intrinsic::StrContains:
        case Intrinsic::StrStartsWith:
        case Intrinsic::StrEndsWith: ok = sig({kStr, kStr}, kBool); break;
        case Intrinsic::StrFind:
            ok = at.size() == 2 && at[0] == kStr && at[1] == kStr && dst != kVoid && t_.kind(dst) == TypeKind::Nullable &&
                 t_.info(dst).elem == kI64;
            break;
        case Intrinsic::StrByteAt: ok = sig({kStr, kI64}, kI64); break;
        case Intrinsic::StrFromI64: ok = sig({kI64}, kStr); break;
        case Intrinsic::StrFromU64: ok = sig({kU64}, kStr); break;
        case Intrinsic::StrFromF64: ok = sig({kF64}, kStr); break;
        case Intrinsic::StrFromBool: ok = sig({kBool}, kStr); break;
        case Intrinsic::StrFromChar: ok = sig({kChar}, kStr); break;
        case Intrinsic::MathSqrt:
        case Intrinsic::MathFloor:
        case Intrinsic::MathCeil:
        case Intrinsic::MathAbsF:
        case Intrinsic::MathSin:
        case Intrinsic::MathCos: ok = sig({kF64}, kF64); break;
        case Intrinsic::MathPow:
        case Intrinsic::MathMinF:
        case Intrinsic::MathMaxF: ok = sig({kF64, kF64}, kF64); break;
        case Intrinsic::MathAbsI: ok = sig({kI64}, kI64); break;
        case Intrinsic::MathMinI:
        case Intrinsic::MathMaxI: ok = sig({kI64, kI64}, kI64); break;
        case Intrinsic::Panic: ok = sig({kStr}, kVoid); break;
        case Intrinsic::Count: break;
        default:
            ok = builtinSignatureOk(t_, op, at, dst);
            break;
        }
        if (!ok) {
            return fail(std::format("bad operand types for intrinsic {}", intrinsicName(op)));
        }
        return {};
    }

    std::expected<void, ValidationError> callArgs(const Inst& in, const std::vector<TypeId>& params, TypeId result) {
        std::span<const Reg> a = args(in);
        if (a.size() != params.size()) {
            return fail("wrong argument count");
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (ty(a[i]) != params[i]) {
                return fail(std::format("argument {} has the wrong type", i));
            }
        }
        if ((result == kVoid) != (in.dst == kNoReg) || (in.dst != kNoReg && (!reg(in.dst) || ty(in.dst) != result))) {
            return fail("call result type mismatch");
        }
        return {};
    }

    const std::vector<Field>* fieldsOf(TypeId t) const {
        const TypeInfo& info = t_.info(t);
        if (info.kind == TypeKind::Struct || info.kind == TypeKind::State) {
            return &info.fields;
        }
        return nullptr;
    }

    std::expected<TypeId, ValidationError> fieldType(TypeId agg, std::uint32_t field) {
        const TypeInfo& info = t_.info(agg);
        if (info.kind == TypeKind::Struct || info.kind == TypeKind::State) {
            if (field < info.fields.size()) {
                return info.fields[field].type;
            }
        } else if (info.kind == TypeKind::Tuple) {
            if (field < info.elems.size()) {
                return info.elems[field];
            }
        } else {
            return fail("field access on a non aggregate");
        }
        return fail("field index out of range");
    }

    std::expected<void, ValidationError> branch(std::uint32_t depth, bool conditional) {
        if (depth >= frames_.size()) {
            return fail("branch depth out of range");
        }
        Frame& target = frames_[frames_.size() - 1 - depth];
        if (!state_.unreachable && state_.scopes != target.entry.scopes) {
            return fail("branch leaves a scope open");
        }
        if (target.op != Op::Loop && !target.exits.meet(state_)) {
            return fail("branch leaves a scope open");
        }
        if (!conditional) {
            state_.unreachable = true;
        }
        return {};
    }

    std::expected<void, ValidationError> step(const Inst& in) {
        if (in.argBegin > fn_.args.size() || in.argCount > fn_.args.size() - in.argBegin) {
            return fail("argument list out of range");
        }
        if (!operandsAllowed(in)) {
            return fail("operand the instruction does not use");
        }
        for (Reg r : args(in)) {
            if (auto u = use(r); !u) {
                return u;
            }
        }
        if (stateFields_ > 0 && !state_.unreachable && !stateWritten() && usesState(in)) {
            return fail("state initializer uses its state before writing every field");
        }
        auto needA = [&]() { return use(in.a); };
        auto needB = [&]() { return use(in.b); };
        switch (in.op) {
        case Op::Block:
        case Op::Loop:
        case Op::If: {
            if (in.op == Op::If) {
                if (auto r = needA(); !r) {
                    return r;
                }
                if (ty(in.a) != kBool) {
                    return fail("if condition must be bool");
                }
            }
            Frame f{in.op, state_, {}, {}, false};
            f.exits.unreachable = true;
            f.exits.init.assign(state_.init.size(), true);
            frames_.push_back(std::move(f));
            return {};
        }
        case Op::Else: {
            if (frames_.empty() || frames_.back().op != Op::If || frames_.back().sawElse) {
                return fail("else without if");
            }
            Frame& f = frames_.back();
            f.sawElse = true;
            f.thenEnd = state_;
            state_ = f.entry;
            return {};
        }
        case Op::End: {
            if (frames_.empty()) {
                return fail("end without block");
            }
            Frame f = std::move(frames_.back());
            frames_.pop_back();
            if (!state_.unreachable && state_.scopes != f.entry.scopes) {
                return fail("block ends with a scope open");
            }
            InitState merged = f.exits;
            bool same = true;
            if (f.op == Op::If) {
                same = merged.meet(f.sawElse ? f.thenEnd : f.entry);
            }
            same = merged.meet(state_) && same;
            if (!same) {
                return fail("paths disagree on open scopes");
            }
            state_ = merged;
            if (state_.unreachable && !f.entry.unreachable && f.op != Op::Loop) {
                // A block whose every path branched elsewhere is still unreachable after it.
            }
            return {};
        }
        case Op::Br: return branch(in.imm, false);
        case Op::BrIf:
            if (auto r = needA(); !r) {
                return r;
            }
            if (ty(in.a) != kBool) {
                return fail("br_if condition must be bool");
            }
            return branch(in.imm, true);
        case Op::Return:
            if (!state_.unreachable && state_.scopes != 0) {
                return fail("return with a scope open");
            }
            if (stateFields_ > 0 && !state_.unreachable && !stateWritten()) {
                return fail("state initializer returns before writing every field");
            }
            if (fn_.result == kVoid) {
                if (in.a != kNoReg) {
                    return fail("void function returns a value");
                }
            } else {
                if (auto r = needA(); !r) {
                    return r;
                }
                if (ty(in.a) != fn_.result) {
                    return fail("return type mismatch");
                }
            }
            state_.unreachable = true;
            return {};
        case Op::Trap:
            if (in.imm > static_cast<std::uint32_t>(TrapCode::HostError)) {
                return fail("unknown trap code");
            }
            state_.unreachable = true;
            return {};
        case Op::Const: {
            if (in.imm >= m_.constants.size() || !reg(in.dst)) {
                return fail("bad constant");
            }
            const Constant& c = m_.constants[in.imm];
            TypeId d = ty(in.dst);
            bool ok = c.type == d && ((c.kind == ConstKind::Int && (t_.isInteger(d) || d == kBool || d == kChar)) ||
                                      (c.kind == ConstKind::Float && t_.isFloat(d)) ||
                                      (c.kind == ConstKind::Str && d == kStr));
            if (!ok) {
                return fail("constant type mismatch");
            }
            if (d == kBool && c.bits > 1) {
                return fail("bool constant out of range");
            }
            return def(in.dst);
        }
        case Op::Move:
            if (auto r = needA(); !r) {
                return r;
            }
            if (!reg(in.dst) || ty(in.dst) != ty(in.a)) {
                return fail("move type mismatch");
            }
            return def(in.dst);
        case Op::Neg:
        case Op::Not:
        case Op::BitNot: {
            if (auto r = needA(); !r) {
                return r;
            }
            TypeId t = ty(in.a);
            bool ok = reg(in.dst) && ty(in.dst) == t &&
                      (in.op == Op::Neg ? t_.isNumeric(t) : in.op == Op::Not ? t == kBool : t_.isInteger(t));
            if (!ok) {
                return fail("unary operand type mismatch");
            }
            return def(in.dst);
        }
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Rem:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr: {
            if (auto r = needA(); !r) {
                return r;
            }
            if (auto r = needB(); !r) {
                return r;
            }
            TypeId t = ty(in.a);
            bool arith = in.op <= Op::Rem;
            bool bits = in.op == Op::BitAnd || in.op == Op::BitOr || in.op == Op::BitXor;
            bool ok = reg(in.dst) && ty(in.dst) == t && ty(in.b) == t &&
                      (arith ? t_.isNumeric(t) : bits ? (t_.isInteger(t) || t == kBool) : t_.isInteger(t));
            if (!ok) {
                return fail("binary operand type mismatch");
            }
            return def(in.dst);
        }
        case Op::Eq:
        case Op::Ne:
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge: {
            if (auto r = needA(); !r) {
                return r;
            }
            if (auto r = needB(); !r) {
                return r;
            }
            TypeId t = ty(in.a);
            bool eq = in.op == Op::Eq || in.op == Op::Ne;
            bool ok = reg(in.dst) && ty(in.dst) == kBool && ty(in.b) == t &&
                      (eq ? equatable(t) : (t_.isNumeric(t) || t == kChar || t == kStr));
            if (!ok) {
                return fail("comparison operand type mismatch");
            }
            return def(in.dst);
        }
        case Op::Convert:
            if (auto r = needA(); !r) {
                return r;
            }
            if (!reg(in.dst) || !convertible(ty(in.a), ty(in.dst))) {
                return fail("invalid conversion");
            }
            return def(in.dst);
        case Op::Call: {
            if (in.imm >= m_.functions.size()) {
                return fail("call to unknown function");
            }
            const Function& callee = m_.functions[in.imm];
            if (callee.isSuspend && !fn_.isSuspend) {
                return fail("call to a suspend function from a normal function");
            }
            std::vector<TypeId> params(callee.regs.begin(), callee.regs.begin() + callee.paramCount);
            if (auto r = callArgs(in, params, callee.result); !r) {
                return r;
            }
            return in.dst == kNoReg ? std::expected<void, ValidationError>{} : def(in.dst);
        }
        case Op::Closure:
            if (!reg(in.dst)) {
                return fail("register out of range");
            }
            if (std::string why = closureMismatch(m_, fn_, in); !why.empty()) {
                return fail(std::move(why));
            }
            return def(in.dst);
        case Op::CallValue: {
            if (auto r = needA(); !r) {
                return r;
            }
            if (t_.kind(ty(in.a)) != TypeKind::Fn) {
                return fail("call.value of a value that is not a function");
            }
            std::vector<TypeId> params = t_.info(ty(in.a)).elems;
            if (auto r = callArgs(in, params, t_.info(ty(in.a)).result); !r) {
                return r;
            }
            return in.dst == kNoReg ? std::expected<void, ValidationError>{} : def(in.dst);
        }
        case Op::CallHost: {
            if (in.imm >= m_.imports.size()) {
                return fail("call to unknown import");
            }
            const HostImport& imp = m_.imports[in.imm];
            if (auto r = callArgs(in, imp.params, imp.result); !r) {
                return r;
            }
            return in.dst == kNoReg ? std::expected<void, ValidationError>{} : def(in.dst);
        }
        case Op::Intrinsic:
            if (in.dst != kNoReg && !reg(in.dst)) {
                return fail("register out of range");
            }
            if (auto r = intrinsic(in); !r) {
                return r;
            }
            return in.dst == kNoReg ? std::expected<void, ValidationError>{} : def(in.dst);
        case Op::FieldGet: {
            if (auto r = needA(); !r) {
                return r;
            }
            auto ft = fieldType(ty(in.a), in.imm);
            if (!ft) {
                return std::unexpected(ft.error());
            }
            if (!reg(in.dst) || ty(in.dst) != *ft) {
                return fail("field type mismatch");
            }
            if (stateFields_ > 0 && in.a == 0 && !state_.unreachable && !state_.init[fn_.regs.size() + in.imm]) {
                return fail("state field read before its initializer writes it");
            }
            return def(in.dst);
        }
        case Op::FieldSet: {
            if (auto r = needA(); !r) {
                return r;
            }
            if (auto r = needB(); !r) {
                return r;
            }
            auto ft = fieldType(ty(in.a), in.imm);
            if (!ft) {
                return std::unexpected(ft.error());
            }
            if (ty(in.b) != *ft) {
                return fail("field type mismatch");
            }
            if (stateFields_ > 0 && in.a == 0 && !state_.unreachable) {
                state_.init[fn_.regs.size() + in.imm] = true;
            }
            return {};
        }
        case Op::Make: {
            if (!reg(in.dst)) {
                return fail("register out of range");
            }
            const TypeInfo& info = t_.info(ty(in.dst));
            std::vector<TypeId> want;
            if (info.kind == TypeKind::Struct) {
                for (const Field& f : info.fields) {
                    want.push_back(f.type);
                }
            } else if (info.kind == TypeKind::Tuple) {
                want = info.elems;
            } else {
                return fail("make of a non aggregate");
            }
            std::span<const Reg> a = args(in);
            if (a.size() != want.size()) {
                return fail("make field count mismatch");
            }
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (ty(a[i]) != want[i]) {
                    return fail("make field type mismatch");
                }
            }
            return def(in.dst);
        }
        case Op::IsNull:
        case Op::Unwrap:
        case Op::Wrap: {
            if (auto r = needA(); !r) {
                return r;
            }
            if (!reg(in.dst)) {
                return fail("register out of range");
            }
            TypeId a = ty(in.a);
            TypeId d = ty(in.dst);
            bool ok = in.op == Op::IsNull   ? (t_.kind(a) == TypeKind::Nullable && d == kBool)
                      : in.op == Op::Unwrap ? (t_.kind(a) == TypeKind::Nullable && t_.info(a).elem == d)
                                            : (t_.kind(d) == TypeKind::Nullable && t_.info(d).elem == a);
            if (!ok) {
                return fail("nullable operand type mismatch");
            }
            return def(in.dst);
        }
        case Op::NullOf:
            if (!reg(in.dst) || t_.kind(ty(in.dst)) != TypeKind::Nullable) {
                return fail("null of a non nullable type");
            }
            return def(in.dst);
        case Op::EnumMake: {
            if (!reg(in.dst) || t_.kind(ty(in.dst)) != TypeKind::Enum) {
                return fail("enum.make of a non enum");
            }
            const TypeInfo& info = t_.info(ty(in.dst));
            if (in.imm >= info.variants.size()) {
                return fail("variant out of range");
            }
            const std::vector<TypeId>& payload = info.variants[in.imm].payload;
            std::span<const Reg> a = args(in);
            if (a.size() != payload.size()) {
                return fail("payload count mismatch");
            }
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (ty(a[i]) != payload[i]) {
                    return fail("payload type mismatch");
                }
            }
            return def(in.dst);
        }
        case Op::EnumTag:
            if (auto r = needA(); !r) {
                return r;
            }
            if (!reg(in.dst) || t_.kind(ty(in.a)) != TypeKind::Enum || ty(in.dst) != kI64) {
                return fail("enum.tag type mismatch");
            }
            return def(in.dst);
        case Op::ScopeEnter:
            if (!state_.unreachable) {
                ++state_.scopes;
            }
            return {};
        case Op::ScopeExit:
            if (in.a > in.b || in.b > fn_.regs.size()) {
                return fail("bad scope register range");
            }
            if (!state_.unreachable) {
                if (state_.scopes == 0) {
                    return fail("scope exit without scope");
                }
                --state_.scopes;
                for (Reg r = in.a; r < in.b; ++r) {
                    state_.init[r] = false;
                }
            }
            return {};
        case Op::Persist:
            if (auto r = needA(); !r) {
                return r;
            }
            if (in.imm == 0 || (!state_.unreachable && in.imm > state_.scopes)) {
                return fail("persist beyond the open scopes");
            }
            return {};
        case Op::Wait: {
            if (!fn_.isSuspend) {
                return fail("wait outside a suspend function");
            }
            if (auto r = needA(); !r) {
                return r;
            }
            if (t_.kind(ty(in.a)) != TypeKind::Handle || in.imm > static_cast<std::uint32_t>(WaitKind::Yield)) {
                return fail("wait needs a handle");
            }
            if (in.dst != kNoReg) {
                if (!reg(in.dst)) {
                    return fail("register out of range");
                }
                TypeId d = ty(in.dst);
                // One slot each: the host hands a single value to `resume`, deep-copied into the task's arena.
                if (!t_.isScalar(d) && d != kStr && t_.kind(d) != TypeKind::Handle && t_.kind(d) != TypeKind::List) {
                    return fail("wait can only produce a scalar, a str, a handle or a list");
                }
                return def(in.dst);
            }
            return {};
        }
        case Op::EnumField: {
            if (auto r = needA(); !r) {
                return r;
            }
            if (!reg(in.dst) || t_.kind(ty(in.a)) != TypeKind::Enum) {
                return fail("enum.field of a non enum");
            }
            const TypeInfo& info = t_.info(ty(in.a));
            if (in.imm >= info.variants.size() || in.b >= info.variants[in.imm].payload.size() ||
                info.variants[in.imm].payload[in.b] != ty(in.dst)) {
                return fail("enum.field out of range or mismatched");
            }
            return def(in.dst);
        }
        }
        return fail("unknown opcode");
    }

    // Fields of a state start zeroed, and a zero str, list or map is a null pointer.
    [[nodiscard]] bool stateWritten() const {
        return std::all_of(state_.init.end() - stateFields_, state_.init.end(), [](bool b) { return b; });
    }

    [[nodiscard]] bool usesState(const Inst& in) const {
        bool fieldAccess = in.op == Op::FieldGet || in.op == Op::FieldSet;
        std::span<const Reg> a = args(in);
        return (in.a == 0 && !fieldAccess && in.op != Op::ScopeExit) ||
               (in.b == 0 && in.op != Op::EnumField && in.op != Op::ScopeExit) || in.dst == 0 ||
               std::ranges::find(a, Reg{0}) != a.end();
    }

    const Module& m_;
    const TypeTable& t_;
    const Function& fn_;
    std::uint32_t index_;
    std::uint32_t stateFields_ = 0;
    std::size_t pc_ = 0;
    InitState state_;
    std::vector<Frame> frames_;
};

} // namespace

std::expected<void, ValidationError> validate(const Module& m) {
    if (auto r = validateTypeGraph(m.types); !r) {
        return r;
    }
    if (auto r = validateTables(m); !r) {
        return r;
    }
    for (std::uint32_t f = 0; f < m.functions.size(); ++f) {
        if (auto r = FunctionValidator(m, f).run(); !r) {
            return r;
        }
    }
    return {};
}

} // namespace quartz::ir
