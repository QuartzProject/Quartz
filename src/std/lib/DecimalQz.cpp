#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kDecimalQzi = R"qz(package org.quartzlang.decimal

/// round(a * b * 10^exp / c) computed exactly, with `mode`; errors on division by zero, overflow and `Unnecessary`.
fn mulDivPow10(a: int, b: int, exp: int, c: int, mode: Rounding) -> Result<int, str>
)qz";

const std::string_view kDecimalQz = R"qz(package org.quartzlang.decimal

/// How a result that does not fit the scale is rounded; the names and rules are those of Java's RoundingMode.
pub enum Rounding {
    Down
    Up
    Ceiling
    Floor
    HalfUp
    HalfDown
    HalfEven
    Unnecessary
}

/// An exact decimal `unscaled / 10^scale` for money and the like: up to 18 digits, scale 0 to 18, no rounding unless
/// asked for. Overflow, division by zero and needed rounding under `Unnecessary` are panics.
pub struct Decimal {
    unscaled: int
    scale: int
}

const MaxScale: int = 18

fn check(r: Result<int, str>) -> int {
    match r {
        Ok(v) => {
            return v
        }
        Err(e) => panic("decimal: {e}")
    }
}

fn must(v: int?) -> int {
    if let x = v {
        return x
    }
    panic("decimal: overflow")
}

fn checkScale(scale: int) {
    if scale < 0 || scale > MaxScale {
        panic("decimal: scale {scale} is outside 0 to {MaxScale}")
    }
}

pub fn decimalOf(value: int) -> Decimal {
    return Decimal(value, 0)
}

/// `unscaled / 10^scale`: `decimalOfUnscaled(1999, 2)` is 19.99.
pub fn decimalOfUnscaled(unscaled: int, scale: int) -> Decimal {
    checkScale(scale)
    return Decimal(unscaled, scale)
}

/// Plain notation (`-12.50`, `0.001`, `7`); the scale is the number of digits after the point.
pub fn parseDecimal(text: str) -> Result<Decimal, str> {
    let n = text.len()
    var i = 0
    var negative = false
    if n > 0 && (text.byteAt(0) == 43 || text.byteAt(0) == 45) {
        negative = text.byteAt(0) == 45
        i = 1
    }
    var value = 0
    var digits = 0
    var scale = -1
    while i < n {
        let c = text.byteAt(i)
        if c == 46 && scale < 0 && digits > 0 {
            scale = 0
        } else if c >= 48 && c <= 57 {
            let d = if negative { 48 - c } else { c - 48 }
            if let v = value.checkedMul(10)?.checkedAdd(d) {
                value = v
            } else {
                return Err("'{text}' has more digits than a decimal holds")
            }
            digits += 1
            if scale >= 0 {
                scale += 1
            }
        } else {
            return Err("'{text}' is not a decimal number")
        }
        i += 1
    }
    if digits == 0 || scale == 0 {
        return Err("'{text}' is not a decimal number")
    }
    let s = if scale < 0 { 0 } else { scale }
    if s > MaxScale {
        return Err("'{text}' has more than {MaxScale} digits after the point")
    }
    return Ok(Decimal(value, s))
}

impl Decimal {
    /// The same number with `scale` digits after the point, rounded with `mode` when digits are dropped.
    pub fn rescale(self, scale: int, mode: Rounding = Rounding.HalfEven) -> Decimal {
        checkScale(scale)
        return Decimal(check(mulDivPow10(self.unscaled, 1, scale - self.scale, 1, mode)), scale)
    }

    fn widen(self, scale: int) -> int {
        return check(mulDivPow10(self.unscaled, 1, scale - self.scale, 1, Rounding.Unnecessary))
    }

    pub fn plus(self, other: Decimal) -> Decimal {
        let s = if self.scale > other.scale { self.scale } else { other.scale }
        return Decimal(must(self.widen(s).checkedAdd(other.widen(s))), s)
    }

    pub fn minus(self, other: Decimal) -> Decimal {
        return self.plus(other.negate())
    }

    /// The exact product; its scale is the sum of the scales, so it must stay at 18 or below.
    pub fn times(self, other: Decimal) -> Decimal {
        let s = self.scale + other.scale
        checkScale(s)
        return Decimal(check(mulDivPow10(self.unscaled, other.unscaled, 0, 1, Rounding.Unnecessary)), s)
    }

    /// The product rounded to `scale` digits; exact in between, so it never rounds twice.
    pub fn timesRounded(self, other: Decimal, scale: int, mode: Rounding = Rounding.HalfEven) -> Decimal {
        checkScale(scale)
        let exp = scale - self.scale - other.scale
        return Decimal(check(mulDivPow10(self.unscaled, other.unscaled, exp, 1, mode)), scale)
    }

    /// The quotient with `scale` digits after the point, rounded with `mode`.
    pub fn div(self, other: Decimal, scale: int, mode: Rounding = Rounding.HalfEven) -> Decimal {
        checkScale(scale)
        let exp = scale + other.scale - self.scale
        return Decimal(check(mulDivPow10(self.unscaled, 1, exp, other.unscaled, mode)), scale)
    }

    pub fn negate(self) -> Decimal {
        return Decimal(must(0.checkedSub(self.unscaled)), self.scale)
    }

    pub fn abs(self) -> Decimal {
        return if self.unscaled < 0 { self.negate() } else { self }
    }

    /// -1, 0 or 1.
    pub fn sign(self) -> int {
        return if self.unscaled < 0 { -1 } else if self.unscaled > 0 { 1 } else { 0 }
    }

    pub fn isZero(self) -> bool {
        return self.unscaled == 0
    }

    /// By value: 1.5 and 1.50 compare equal.
    pub fn compareTo(self, other: Decimal) -> int {
        let s = if self.scale > other.scale { self.scale } else { other.scale }
        let a = mulDivPow10(self.unscaled, 1, s - self.scale, 1, Rounding.Unnecessary)
        let b = mulDivPow10(other.unscaled, 1, s - other.scale, 1, Rounding.Unnecessary)
        // a value too large to widen is larger in magnitude than anything at the smaller scale
        if a.isErr() {
            return self.sign()
        }
        if b.isErr() {
            return -other.sign()
        }
        let x = a.unwrap()
        let y = b.unwrap()
        return if x < y { -1 } else if x > y { 1 } else { 0 }
    }

    pub fn equals(self, other: Decimal) -> bool {
        return self.compareTo(other) == 0
    }

    /// Plain notation with exactly `scale` digits after the point.
    pub fn toStr(self) -> str {
        let negative = self.unscaled < 0
        var digits = if negative { "{(0 as u64) - (self.unscaled as u64)}" } else { "{self.unscaled}" }
        if self.scale == 0 {
            return if negative { "-" + digits } else { digits }
        }
        while digits.len() <= self.scale {
            digits = "0" + digits
        }
        let point = digits.len() - self.scale
        let body = "{digits.slice(0, point)}.{digits.slice(point, digits.len())}"
        return if negative { "-" + body } else { body }
    }

    /// The nearest float, correctly rounded.
    pub fn toFloat(self) -> float {
        return self.toStr().toFloat() ?? 0.0
    }
}
)qz";

} // namespace quartz::stdlib
