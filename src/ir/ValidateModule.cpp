#include "ir/Validator.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace quartz::ir {

namespace {

constexpr std::uint8_t kDst = 1;
constexpr std::uint8_t kA = 2;
constexpr std::uint8_t kB = 4;

/// Operand fields an opcode reads or writes as registers; every other field must be kNoReg.
std::uint8_t registerOperands(Op op) {
    switch (op) {
    case Op::Block:
    case Op::Loop:
    case Op::ScopeEnter:
    case Op::Else:
    case Op::End:
    case Op::Br:
    case Op::Trap:
        return 0;
    case Op::If:
    case Op::BrIf:
    case Op::Return:
    case Op::Persist:
        return kA;
    case Op::Const:
    case Op::NullOf:
    case Op::Make:
    case Op::EnumMake:
    case Op::Call:
    case Op::CallHost:
    case Op::Intrinsic:
    case Op::Closure:
        return kDst;
    case Op::FieldSet:
    case Op::ScopeExit:
        return kA | kB;
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::Rem:
    case Op::BitAnd:
    case Op::BitOr:
    case Op::BitXor:
    case Op::Shl:
    case Op::Shr:
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
        return kDst | kA | kB;
    default:
        return kDst | kA;
    }
}

} // namespace

bool operandsAllowed(const Inst& in) {
    std::uint8_t used = registerOperands(in.op);
    return ((used & kDst) || in.dst == kNoReg) && ((used & kA) || in.a == kNoReg) &&
           ((used & kB) || in.b == kNoReg || in.op == Op::EnumField);
}

// Every function value is made here, so a value of a function type always holds a function of that type.
std::string closureMismatch(const Module& m, const Function& fn, const Inst& in) {
    if (in.imm >= m.functions.size()) {
        return "closure of an unknown function";
    }
    const Function& target = m.functions[in.imm];
    if (target.isSuspend) {
        return "a suspend function cannot be a value";
    }
    if (in.argCount > target.paramCount) {
        return "closure captures more values than its function has parameters";
    }
    std::size_t arity = target.paramCount - in.argCount;
    for (std::uint32_t i = 0; i < in.argCount; ++i) {
        if (fn.regs[fn.args[in.argBegin + i]] != target.regs[arity + i]) {
            return std::format("capture {} has the wrong type", i);
        }
    }
    const TypeInfo& d = m.types.info(fn.regs[in.dst]);
    if (d.kind != TypeKind::Fn || d.result != target.result || d.elems.size() != arity ||
        !std::equal(d.elems.begin(), d.elems.end(), target.regs.begin())) {
        return "closure type does not match its function";
    }
    return {};
}

std::expected<void, ValidationError> validateTables(const Module& m) {
    auto valueType = [&](TypeId t) { return t < m.types.size() && t != kError && t != kNull && t != kNever; };
    // The host can neither build nor call a function value, so none crosses the boundary.
    for (const HostImport& imp : m.imports) {
        bool ok = valueType(imp.result) && !m.types.containsFn(imp.result);
        for (TypeId p : imp.params) {
            ok = ok && valueType(p) && p != kVoid && !m.types.containsFn(p);
        }
        if (!ok) {
            return std::unexpected(ValidationError{~0U, ~0U, "import with invalid type"});
        }
    }
    for (std::uint32_t f = 0; f < m.functions.size(); ++f) {
        const Function& fn = m.functions[f];
        if (fn.paramCount > fn.regs.size()) {
            return std::unexpected(ValidationError{f, ~0U, "more parameters than registers"});
        }
        bool fnSignature = fn.result < m.types.size() && m.types.containsFn(fn.result);
        for (std::uint32_t p = 0; p < fn.paramCount; ++p) {
            fnSignature = fnSignature || (fn.regs[p] < m.types.size() && m.types.containsFn(fn.regs[p]));
        }
        if (fn.exported && fnSignature) {
            return std::unexpected(ValidationError{f, ~0U, "exported function takes or returns a function value"});
        }
        for (const LocalName& l : fn.locals) {
            if (l.reg >= fn.regs.size() || l.from > l.to || l.to > fn.code.size()) {
                return std::unexpected(ValidationError{f, ~0U, "local name outside its function"});
            }
        }
    }
    for (const Annotation& a : m.annotations) {
        if (a.function >= m.functions.size() || (a.filter != ~0U && a.filter >= m.functions.size())) {
            return std::unexpected(ValidationError{~0U, ~0U, "annotation on an unknown function"});
        }
    }
    if (m.states.size() != m.stateInit.size()) {
        return std::unexpected(ValidationError{~0U, ~0U, "state table mismatch"});
    }
    for (std::size_t i = 0; i < m.states.size(); ++i) {
        TypeId s = m.states[i];
        std::uint32_t init = m.stateInit[i];
        if (s >= m.types.size() || m.types.kind(s) != TypeKind::State || init >= m.functions.size() ||
            m.functions[init].paramCount != 1 || m.functions[init].regs[0] != s) {
            return std::unexpected(ValidationError{~0U, ~0U, "bad state initializer"});
        }
        // A state outlives the invocation and survives reloads; a function value would point into either.
        if (m.types.containsFn(s)) {
            return std::unexpected(ValidationError{~0U, ~0U, "state holds a function value"});
        }
    }
    std::vector<std::uint32_t> instances(m.types.size(), 0);
    for (TypeId s : m.states) {
        ++instances[s];
    }
    for (TypeId t = kFirstUserType; t < m.types.size(); ++t) {
        if ((m.types.kind(t) == TypeKind::State) != (instances[t] == 1)) {
            return std::unexpected(ValidationError{~0U, ~0U, "every state type needs exactly one instance"});
        }
    }
    return {};
}

} // namespace quartz::ir
