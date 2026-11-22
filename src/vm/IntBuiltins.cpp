#include "ir/Types.hpp"
#include "support/Int128.hpp"
#include "vm/Builtins.hpp"

namespace quartz::vm {

namespace {

using i128 = __int128;
using u128 = unsigned __int128;

struct Range {
    bool isSigned;
    i128 lo;
    i128 hi;
};

// A type id the validator did not pin to an integer type (hostile IR) reads as int: wrong numbers, never unsafety.
Range rangeOf(const Slot& code) {
    switch (code.u) {
    case ir::kI8:
        return {true, -128, 127};
    case ir::kI16:
        return {true, -32768, 32767};
    case ir::kI32:
        return {true, -(i128{1} << 31), (i128{1} << 31) - 1};
    case ir::kU8:
        return {false, 0, 0xFF};
    case ir::kU16:
        return {false, 0, 0xFFFF};
    case ir::kU32:
        return {false, 0, 0xFFFFFFFF};
    case ir::kU64:
        return {false, 0, static_cast<i128>(~std::uint64_t{0})};
    default:
        return {true, -(i128{1} << 63), (i128{1} << 63) - 1};
    }
}

i128 operand(const Slot& s, const Range& r) { return r.isSigned ? i128{s.i} : i128{s.u}; }

enum class Arith { Add, Sub, Mul, Div };

// False when the exact result does not exist (division by zero) or does not fit the type.
bool exact(const Slot* a, Arith op, const Range& r, i128& out) {
    i128 x = operand(a[0], r);
    i128 y = operand(a[1], r);
    switch (op) {
    case Arith::Add:
        out = x + y;
        break;
    case Arith::Sub:
        out = x - y;
        break;
    case Arith::Mul:
        if (!r.isSigned) {
            u128 p = static_cast<u128>(x) * static_cast<u128>(y);
            if (p > static_cast<u128>(r.hi)) {
                return false;
            }
            out = static_cast<i128>(p);
            return true;
        }
        out = x * y;
        break;
    case Arith::Div:
        if (y == 0) {
            return false;
        }
        i128 rest = 0;
        out = support::divmod(x, y, rest);
        break;
    }
    return out >= r.lo && out <= r.hi;
}

bool checked(const Slot* a, Slot* out, Arith op) {
    Range r = rangeOf(a[2]);
    i128 v = 0;
    bool ok = exact(a, op, r, v);
    out[0].u = ok ? 1 : 0;
    out[1].u = ok ? static_cast<std::uint64_t>(v) : 0;
    return true;
}

bool saturating(const Slot* a, Slot* out, Arith op) {
    Range r = rangeOf(a[2]);
    i128 v = 0;
    if (!exact(a, op, r, v)) {
        i128 x = operand(a[0], r);
        i128 y = operand(a[1], r);
        bool negative = op == Arith::Mul ? (x < 0) != (y < 0) : op == Arith::Sub ? x < y : x + y < 0;
        v = negative ? r.lo : r.hi;
    }
    out[0].u = static_cast<std::uint64_t>(v);
    return true;
}

} // namespace

bool Builtins::checkedAdd(Sandbox&, const Slot* a, Slot* out) { return checked(a, out, Arith::Add); }

bool Builtins::checkedSub(Sandbox&, const Slot* a, Slot* out) { return checked(a, out, Arith::Sub); }

bool Builtins::checkedMul(Sandbox&, const Slot* a, Slot* out) { return checked(a, out, Arith::Mul); }

bool Builtins::checkedDiv(Sandbox&, const Slot* a, Slot* out) { return checked(a, out, Arith::Div); }

bool Builtins::saturatingAdd(Sandbox&, const Slot* a, Slot* out) { return saturating(a, out, Arith::Add); }

bool Builtins::saturatingSub(Sandbox&, const Slot* a, Slot* out) { return saturating(a, out, Arith::Sub); }

bool Builtins::saturatingMul(Sandbox&, const Slot* a, Slot* out) { return saturating(a, out, Arith::Mul); }

} // namespace quartz::vm
