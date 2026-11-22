#include "std/Natives.hpp"
#include "support/Int128.hpp"

namespace quartz::stdlib {

namespace {

using i128 = __int128;

enum class Rounding : std::uint64_t { Down, Up, Ceiling, Floor, HalfUp, HalfDown, HalfEven, Unnecessary };

using u128 = unsigned __int128;

// Not __builtin_mul_overflow: that needs __muloti4 from compiler-rt, which libgcc does not provide.
bool mulOverflows(i128 a, i128 b, i128& out) {
    if (a == 0 || b == 0) {
        out = 0;
        return false;
    }
    const bool negative = (a < 0) != (b < 0);
    const u128 ua = a < 0 ? u128{0} - static_cast<u128>(a) : static_cast<u128>(a);
    const u128 ub = b < 0 ? u128{0} - static_cast<u128>(b) : static_cast<u128>(b);
    const u128 limit = (u128{1} << 127) - (negative ? 0 : 1);
    if (ua > limit / ub) {
        out = 0;
        return true;
    }
    const u128 product = ua * ub;
    out = static_cast<i128>(negative ? u128{0} - product : product);
    return false;
}

// round(a * b * 10^exp / c) exactly, for the `Rounding` variant `mode`; the core of Decimal arithmetic.
bool mulDivPow10(vm::HostCall& call, void*) {
    i128 n = call.args[0].i;
    std::int64_t exp = call.args[2].i;
    i128 d = call.args[3].i;
    auto mode = static_cast<Rounding>(call.args[4].u);
    if (d == 0) {
        return errStr(call, "division by zero");
    }
    if (exp < -38 || exp > 38) {
        return errStr(call, "exponent out of range");
    }
    bool overflow = mulOverflows(n, call.args[1].i, n);
    for (std::int64_t k = 0; k < exp && !overflow; ++k) {
        overflow = mulOverflows(n, 10, n);
    }
    if (overflow) {
        return errStr(call, "decimal overflow");
    }
    // A denominator past i128 makes |quotient| < 1/2: only the sign of a non-zero remainder matters then.
    bool tiny = false;
    for (std::int64_t k = 0; k < -exp && !tiny; ++k) {
        tiny = mulOverflows(d, 10, d);
    }
    bool negative = (n < 0) != (d < 0);
    i128 r = n;
    i128 q = tiny ? 0 : support::divmod(n, d, r);
    if (r != 0) {
        int half = -1;
        if (!tiny) {
            i128 twice = r < 0 ? -2 * r : 2 * r;
            i128 whole = d < 0 ? -d : d;
            half = twice < whole ? -1 : twice > whole ? 1 : 0;
        }
        bool away = false;
        switch (mode) {
        case Rounding::Down:
            break;
        case Rounding::Up:
            away = true;
            break;
        case Rounding::Ceiling:
            away = !negative;
            break;
        case Rounding::Floor:
            away = negative;
            break;
        case Rounding::HalfUp:
            away = half >= 0;
            break;
        case Rounding::HalfDown:
            away = half > 0;
            break;
        case Rounding::HalfEven:
            away = half > 0 || (half == 0 && (q & 1) != 0);
            break;
        case Rounding::Unnecessary:
        default:
            return errStr(call, "rounding necessary");
        }
        if (away) {
            q += negative ? -1 : 1;
        }
    }
    if (q > INT64_MAX || q < INT64_MIN) {
        return errStr(call, "decimal overflow");
    }
    return okScalar(call, static_cast<std::uint64_t>(static_cast<std::int64_t>(q)));
}

} // namespace

void addDecimalNatives(std::vector<Native>& out) { out.push_back({"org.quartzlang.decimal.mulDivPow10", mulDivPow10}); }

} // namespace quartz::stdlib
