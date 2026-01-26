#include "vm/Math.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace quartz::vm::math {

namespace {

// pi/2 in three parts: the first two keep 33 significant bits so k * part is exact for |k| < 2^20.
constexpr double kPio2A = 0x1.921fb544p+0;
constexpr double kPio2B = 0x1.0b4611a6p-34;
constexpr double kPio2C = 0x1.3198a2e037073p-69;
constexpr double kTwoOverPi = 0x1.45f306dc9c883p-1;
constexpr double kTwoPi = 0x1.921fb54442d18p+2;
constexpr double kReduceLimit = 0x1p+20;
constexpr double kLn2Hi = 0x1.62e42ffp-1;
constexpr double kLn2Lo = -0x1.718432a1b0e26p-35;
constexpr double kInvLn2 = 0x1.71547652b82fep+0;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// Taylor coefficients are exactly 1/n!, computed here instead of written down.
template <int N>
struct Factorials {
    double inv[N + 1]{};
    constexpr Factorials() {
        double f = 1;
        inv[0] = 1;
        for (int i = 1; i <= N; ++i) {
            f *= i;
            inv[i] = 1.0 / f;
        }
    }
};
constexpr Factorials<20> kFact;

// sin(r) for |r| <= pi/4: the dropped term r^19/19! is below 1e-19 relative.
double sinPoly(double r) {
    double z = r * r;
    double p = kFact.inv[17];
    p = p * z - kFact.inv[15];
    p = p * z + kFact.inv[13];
    p = p * z - kFact.inv[11];
    p = p * z + kFact.inv[9];
    p = p * z - kFact.inv[7];
    p = p * z + kFact.inv[5];
    p = p * z - kFact.inv[3];
    return r + r * z * p;
}

double cosPoly(double r) {
    double z = r * r;
    double p = -kFact.inv[18];
    p = p * z + kFact.inv[16];
    p = p * z - kFact.inv[14];
    p = p * z + kFact.inv[12];
    p = p * z - kFact.inv[10];
    p = p * z + kFact.inv[8];
    p = p * z - kFact.inv[6];
    p = p * z + kFact.inv[4];
    double half = 0.5 * z;
    return (1.0 - half) + (((1.0 - (1.0 - half)) - half) + z * z * p);
}

// Returns the quadrant and r = x - k*pi/2 with |r| <= pi/4 (approximately).
int reduce(double x, double& r) {
    if (std::fabs(x) > kReduceLimit) {
        x = std::fmod(x, kTwoPi); // exact, so still deterministic; huge arguments lose meaning anyway
    }
    double k = std::floor(x * kTwoOverPi + 0.5);
    r = ((x - k * kPio2A) - k * kPio2B) - k * kPio2C;
    auto q = static_cast<std::int64_t>(k);
    return static_cast<int>(q & 3);
}

// Double-double helpers (Dekker): exact without FMA because the build disables contraction.
struct DD {
    double hi;
    double lo;
};

DD twoSum(double a, double b) {
    double s = a + b;
    double bb = s - a;
    double e = (a - (s - bb)) + (b - bb);
    return {s, e};
}

DD split(double a) {
    constexpr double kSplit = 134217729.0; // 2^27 + 1
    double c = kSplit * a;
    double hi = c - (c - a);
    return {hi, a - hi};
}

DD twoProd(double a, double b) {
    double p = a * b;
    DD sa = split(a);
    DD sb = split(b);
    double e = ((sa.hi * sb.hi - p) + sa.hi * sb.lo + sa.lo * sb.hi) + sa.lo * sb.lo;
    return {p, e};
}

DD ddMul(DD a, DD b) {
    DD p = twoProd(a.hi, b.hi);
    p.lo += a.hi * b.lo + a.lo * b.hi;
    return twoSum(p.hi, p.lo);
}

DD ddAdd(DD a, DD b) {
    DD s = twoSum(a.hi, b.hi);
    s.lo += a.lo + b.lo;
    return twoSum(s.hi, s.lo);
}

DD ddDiv(DD a, DD b) {
    double q1 = a.hi / b.hi;
    DD r = ddAdd(a, ddMul({-q1, 0}, b));
    double q2 = r.hi / b.hi;
    return twoSum(q1, q2);
}

// log(x) for finite x > 0 as a double-double: log(m * 2^e) with m in [sqrt(1/2), sqrt(2)).
DD logDD(double x) {
    int e = 0;
    double m = std::frexp(x, &e);
    if (m < 0x1.6a09e667f3bcdp-1) {
        m *= 2;
        --e;
    }
    // log(m) = 2 atanh(s), s = (m - 1) / (m + 1), |s| <= 0.1716
    DD s = ddDiv(twoSum(m, -1.0), twoSum(m, 1.0));
    DD s2 = ddMul(s, s);
    double z = s2.hi;
    double tail = 1.0 / 23;
    for (int k = 21; k >= 5; k -= 2) {
        tail = tail * z + 1.0 / k;
    }
    constexpr double kThird = 1.0 / 3;
    DD three = twoProd(3.0, kThird);
    DD third{kThird, -((three.hi - 1.0) + three.lo) / 3};
    DD series = ddAdd({1.0, 0}, ddMul(s2, ddAdd(third, {tail * z, 0})));
    DD logm = ddMul({2 * s.hi, 2 * s.lo}, series);
    DD ek = ddAdd(twoProd(static_cast<double>(e), kLn2Hi), twoProd(static_cast<double>(e), kLn2Lo));
    return ddAdd(ek, logm);
}

// exp of a double-double; the caller handles overflow and underflow.
double expDD(DD a) {
    double k = std::floor(a.hi * kInvLn2 + 0.5);
    DD r = ddAdd(a, ddAdd(twoProd(-k, kLn2Hi), twoProd(-k, kLn2Lo)));
    double x = r.hi;
    double q = kFact.inv[15];
    for (int n = 14; n >= 2; --n) {
        q = q * x + kFact.inv[n];
    }
    // exp(x) = 1 + x + x^2 q: the leading 1 + x is kept exact so only the small tail is rounded.
    DD lead = twoSum(1.0, x);
    double tail = x * x * q + r.lo * (1.0 + x);
    DD ex = ddAdd(lead, {tail, 0});
    return std::ldexp(ex.hi + ex.lo, static_cast<int>(k));
}

bool isOddInteger(double y) {
    if (std::fabs(y) >= 0x1p53) {
        return false;
    }
    double h = y * 0.5;
    return std::floor(y) == y && std::floor(h) != h;
}

} // namespace

double sin(double x) {
    if (!std::isfinite(x)) {
        return kNaN;
    }
    if (std::fabs(x) < 0x1p-27) {
        return x;
    }
    double r = 0;
    switch (reduce(x, r)) {
    case 0: return sinPoly(r);
    case 1: return cosPoly(r);
    case 2: return -sinPoly(r);
    default: return -cosPoly(r);
    }
}

double cos(double x) {
    if (!std::isfinite(x)) {
        return kNaN;
    }
    if (std::fabs(x) < 0x1p-27) {
        return 1.0;
    }
    double r = 0;
    switch (reduce(x, r)) {
    case 0: return cosPoly(r);
    case 1: return -sinPoly(r);
    case 2: return -cosPoly(r);
    default: return sinPoly(r);
    }
}

double pow(double x, double y) {
    // IEEE 754 special cases, in the order the standard lists them.
    if (y == 0) {
        return 1.0;
    }
    if (x == 1.0) {
        return 1.0;
    }
    if (std::isnan(x) || std::isnan(y)) {
        return kNaN;
    }
    if (std::isinf(y)) {
        double ax = std::fabs(x);
        if (ax == 1.0) {
            return 1.0;
        }
        return (ax > 1.0) == (y > 0) ? kInf : 0.0;
    }
    bool odd = isOddInteger(y);
    if (x == 0) {
        if (y < 0) {
            return odd ? std::copysign(kInf, x) : kInf;
        }
        return odd ? x : 0.0;
    }
    if (std::isinf(x)) {
        if (x > 0) {
            return y > 0 ? kInf : 0.0;
        }
        double mag = y > 0 ? kInf : 0.0;
        return odd ? -mag : mag;
    }
    double sign = 1.0;
    if (x < 0) {
        if (std::floor(y) != y) {
            return kNaN;
        }
        sign = odd ? -1.0 : 1.0;
        x = -x;
    }
    if (x == 1.0) {
        return sign;
    }
    if (std::fabs(y) > 0x1p62) {
        return x > 1.0 == (y > 0) ? sign * kInf : sign * 0.0; // |y log x| is far beyond the range of exp
    }
    if (std::floor(y) == y && std::fabs(y) <= 64) {
        // Small integer powers by squaring in double-double: pow(x, 2) is exactly x * x.
        auto n = static_cast<std::int64_t>(std::fabs(y));
        DD acc{1.0, 0};
        DD base{x, 0};
        while (n > 0) {
            if ((n & 1) != 0) {
                acc = ddMul(acc, base);
            }
            n >>= 1;
            if (n > 0) {
                base = ddMul(base, base);
            }
        }
        if (std::isfinite(acc.hi) && acc.hi != 0) {
            DD r = y < 0 ? ddDiv({1.0, 0}, acc) : acc;
            double v = r.hi + r.lo;
            if (std::isfinite(v) && std::fabs(v) >= 0x1p-1022) {
                return sign * v;
            }
        }
    }
    DD l = logDD(x);
    DD t = ddMul(l, {y, 0});
    if (t.hi > 709.8) {
        return sign * kInf;
    }
    if (t.hi < -745.2) {
        return sign * 0.0;
    }
    return sign * expDD(t);
}

} // namespace quartz::vm::math
