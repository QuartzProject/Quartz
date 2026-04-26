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

// 1/n for n up to 23, computed here; the same values the divisions gave when written inline.
struct Reciprocals {
    double inv[24]{};

    constexpr Reciprocals() {
        for (int i = 1; i < 24; ++i) {
            inv[i] = 1.0 / i;
        }
    }
};

constexpr Reciprocals kRecip;

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
    double tail = kRecip.inv[23];
    for (int k = 21; k >= 5; k -= 2) {
        tail = tail * z + kRecip.inv[k];
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

// Derived with exact decimal arithmetic (Machin's formula, atanh series): the rounded value and the rest.
constexpr DD kPio2DD{0x1.921fb54442d18p+0, 0x1.1a62633145c07p-54};
constexpr DD kPiDD{0x1.921fb54442d18p+1, 0x1.1a62633145c07p-53};
constexpr DD kInvLn10{0x1.bcb7b1526e50ep-2, 0x1.95355baaafad3p-57};
// atan(k/8) for k = 0..8.
constexpr DD kAtanEighths[] = {
    {0.0, 0.0},
    {0x1.fd5ba9aac2f6ep-4, -0x1.cd37686760c17p-59},
    {0x1.f5b75f92c80ddp-3, 0x1.8ab6e3cf7afbdp-57},
    {0x1.6f61941e4def1p-2, -0x1.c63aae6f6e918p-56},
    {0x1.dac670561bb4fp-2, 0x1.a2b7f222f65e2p-56},
    {0x1.1e00babdefeb4p-1, -0x1.928df287a668fp-58},
    {0x1.4978fa3269ee1p-1, 0x1.2419a87f2a458p-56},
    {0x1.700a7c5784634p-1, -0x1.8c34d25aadef6p-56},
    {0x1.921fb54442d18p-1, 0x1.1a62633145c07p-55},
};

DD ddNeg(DD a) { return {-a.hi, -a.lo}; }

DD ddSqrt(DD a) {
    double r = std::sqrt(a.hi);
    if (r == 0) {
        return {r, 0};
    }
    DD e = ddAdd(a, ddNeg(twoProd(r, r)));
    return twoSum(r, e.hi / (2 * r));
}

// atan(w) for 0 <= w <= 1: atan(c) + atan(t) with c = k/8 nearest and t = (w - c) / (1 + w c), |t| <= 1/15.
DD atanUnit(DD w) {
    auto k = static_cast<int>(std::floor(w.hi * 8 + 0.5));
    double c = k / 8.0;
    DD t = k == 0 ? w : ddDiv(ddAdd(w, {-c, 0}), ddAdd({1.0, 0}, ddMul(w, {c, 0})));
    double z = t.hi * t.hi;
    double p = -kRecip.inv[19];
    for (int n = 17; n >= 3; n -= 2) {
        p = p * z + ((n / 2) % 2 == 0 ? kRecip.inv[n] : -kRecip.inv[n]);
    }
    DD series = ddAdd(t, {t.hi * z * p, 0});
    return ddAdd(kAtanEighths[k], series);
}

// atan(z) for z >= 0 finite, through atan(z) = pi/2 - atan(1/z) above 1.
DD atanPositive(DD z) {
    if (z.hi <= 1.0) {
        return atanUnit(z);
    }
    return ddAdd(kPio2DD, ddNeg(atanUnit(ddDiv({1.0, 0}, z))));
}

// atan(|y| / |x|) for finite nonzero operands, scaled so no intermediate overflows.
DD atanRatio(double ay, double ax) {
    int ey = 0;
    int ex = 0;
    double my = std::frexp(ay, &ey);
    double mx = std::frexp(ax, &ex);
    int d = ey - ex;
    if (d > 60) {
        return ddAdd(kPio2DD, {-(ax / ay), 0});
    }
    if (d < -60) {
        return {ay / ax, 0};
    }
    DD q = ddDiv({my, 0}, {mx, 0});
    return atanPositive({std::ldexp(q.hi, d), std::ldexp(q.lo, d)});
}

DD sinDD(double r) {
    double z = r * r;
    double p = kFact.inv[17];
    p = p * z - kFact.inv[15];
    p = p * z + kFact.inv[13];
    p = p * z - kFact.inv[11];
    p = p * z + kFact.inv[9];
    p = p * z - kFact.inv[7];
    p = p * z + kFact.inv[5];
    p = p * z - kFact.inv[3];
    return twoSum(r, r * z * p);
}

DD cosDD(double r) {
    DD z = twoProd(r, r);
    double p = -kFact.inv[18];
    p = p * z.hi + kFact.inv[16];
    p = p * z.hi - kFact.inv[14];
    p = p * z.hi + kFact.inv[12];
    p = p * z.hi - kFact.inv[10];
    p = p * z.hi + kFact.inv[8];
    p = p * z.hi - kFact.inv[6];
    p = p * z.hi + kFact.inv[4];
    DD lead = twoSum(1.0, -0.5 * z.hi);
    return twoSum(lead.hi, (lead.lo - 0.5 * z.lo) + z.hi * z.hi * p);
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

double tan(double x) {
    if (!std::isfinite(x)) {
        return kNaN;
    }
    if (std::fabs(x) < 0x1p-27) {
        return x;
    }
    if (std::fabs(x) > kReduceLimit) {
        x = std::fmod(x, kTwoPi);
    }
    // The reduction of reduce(), kept as a double-double: x - k*A is exact for |k| < 2^20 (Sterbenz).
    double k = std::floor(x * kTwoOverPi + 0.5);
    DD r = twoSum(x - k * kPio2A, -k * kPio2B);
    r = twoSum(r.hi, r.lo - k * kPio2C);
    DD s = sinDD(r.hi);
    DD c = cosDD(r.hi);
    // sin(h + l) = sin h + l cos h and cos(h + l) = cos h - l sin h, to first order in the tiny l.
    s = ddAdd(s, {r.lo * c.hi, 0});
    c = ddAdd(c, {-r.lo * s.hi, 0});
    return (static_cast<std::int64_t>(k) & 1) == 0 ? ddDiv(s, c).hi : -ddDiv(c, s).hi;
}

double atan(double x) {
    if (std::isnan(x)) {
        return x;
    }
    double ax = std::fabs(x);
    if (ax < 0x1p-27) {
        return x;
    }
    DD r = std::isinf(x) ? kPio2DD : ax > 0x1p60 ? ddAdd(kPio2DD, {-(1.0 / ax), 0}) : atanPositive({ax, 0});
    return std::copysign(r.hi, x);
}

// The special cases follow C99 Annex F (F.9.1.4).
double atan2(double y, double x) {
    if (std::isnan(x) || std::isnan(y)) {
        return kNaN;
    }
    bool xNeg = std::signbit(x);
    DD r{0, 0};
    if (y == 0) {
        r = xNeg ? kPiDD : DD{0, 0};
    } else if (std::isinf(y)) {
        r = std::isinf(x) ? (xNeg ? ddMul(kPiDD, {0.75, 0}) : ddMul(kPiDD, {0.25, 0})) : kPio2DD;
    } else if (x == 0) {
        r = kPio2DD;
    } else if (std::isinf(x)) {
        r = xNeg ? kPiDD : DD{0, 0};
    } else {
        r = atanRatio(std::fabs(y), std::fabs(x));
        if (xNeg) {
            r = ddAdd(kPiDD, ddNeg(r));
        }
    }
    return std::copysign(r.hi, y);
}

double asin(double x) {
    double ax = std::fabs(x);
    if (std::isnan(x) || ax > 1.0) {
        return kNaN;
    }
    if (ax < 0x1p-27) {
        return x;
    }
    if (ax == 1.0) {
        return std::copysign(kPio2DD.hi, x);
    }
    // asin(x) = atan(x / sqrt(1 - x^2)), with 1 - x^2 exact in double-double.
    DD c = ddSqrt(ddAdd({1.0, 0}, ddNeg(twoProd(ax, ax))));
    return std::copysign(atanPositive(ddDiv({ax, 0}, c)).hi, x);
}

double acos(double x) {
    double ax = std::fabs(x);
    if (std::isnan(x) || ax > 1.0) {
        return kNaN;
    }
    if (ax < 0x1p-60) {
        return ddAdd(kPio2DD, {-x, 0}).hi;
    }
    if (x == 1.0) {
        return 0.0;
    }
    DD c = ddSqrt(ddAdd({1.0, 0}, ddNeg(twoProd(ax, ax))));
    DD r = atanPositive(ddDiv(c, {ax, 0}));
    return x < 0 ? ddAdd(kPiDD, ddNeg(r)).hi : r.hi;
}

double exp(double x) {
    if (std::isnan(x)) {
        return x;
    }
    if (x > 709.8) {
        return kInf;
    }
    if (x < -745.2) {
        return 0.0;
    }
    return expDD({x, 0});
}

double log(double x) {
    if (std::isnan(x) || x < 0) {
        return kNaN;
    }
    if (x == 0) {
        return -kInf;
    }
    if (std::isinf(x)) {
        return x;
    }
    return logDD(x).hi;
}

double log10(double x) {
    if (std::isnan(x) || x < 0 || x == 0 || std::isinf(x)) {
        return log(x);
    }
    return ddMul(logDD(x), kInvLn10).hi;
}

double round(double x) { return std::round(x); }

const UnaryFn kUnary[8] = {tan, asin, acos, atan, exp, log, log10, round};

} // namespace quartz::vm::math
