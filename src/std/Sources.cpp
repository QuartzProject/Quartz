#include "std/Std.hpp"
#include "std/lib/Library.hpp"

namespace quartz::stdlib {

namespace {

constexpr std::string_view kDataQz = R"(package org.quartzlang.data

pub enum Value {
    Null
    Bool(bool)
    Int(int)
    Float(float)
    Str(str)
    List([Value])
    Table(Map<str, Value>)
}

impl Value {
    pub fn get(self, key: str) -> Value? {
        return match self {
            Table(t) => t.get(key)
            _ => null
        }
    }

    pub fn item(self, index: int) -> Value? {
        return match self {
            List(items) => if index >= 0 && index < items.len() { items[index] } else { null }
            _ => null
        }
    }

    pub fn len(self) -> int {
        return match self {
            List(items) => items.len()
            Table(t) => t.len()
            Str(s) => s.len()
            _ => 0
        }
    }

    pub fn isNull(self) -> bool {
        return match self {
            Null => true
            _ => false
        }
    }

    pub fn asBool(self) -> bool? {
        return match self {
            Bool(b) => b
            _ => null
        }
    }

    pub fn asInt(self) -> int? {
        return match self {
            Int(i) => i
            _ => null
        }
    }

    pub fn asFloat(self) -> float? {
        return match self {
            Float(f) => f
            Int(i) => i as float
            _ => null
        }
    }

    pub fn asStr(self) -> str? {
        return match self {
            Str(s) => s
            _ => null
        }
    }
}
)";

constexpr std::string_view kDataQzi = R"(package org.quartzlang.data

fn parseJson(text: str) -> Result<Value, str>
fn toJson(value: Value, indent: int = 0) -> str
fn parseToml(text: str) -> Result<Value, str>
fn parseYaml(text: str) -> Result<Value, str>
)";

constexpr std::string_view kBytesQzi = R"(package org.quartzlang.bytes

impl Bytes {
    fn len(self) -> int
    fn get(self, index: int) -> int
    fn slice(self, begin: int, end: int) -> Bytes
    fn toStr(self) -> str?
    fn toList(self) -> [int]
    fn hex(self) -> str
}

impl str {
    fn toBytes(self) -> Bytes
}

fn bytesOf(values: [int]) -> Bytes
fn concatBytes(parts: [Bytes]) -> Bytes
fn readUInt(data: Bytes, offset: int, size: int, bigEndian: bool = false) -> int
fn writeUInt(data: Bytes, offset: int, size: int, value: int, bigEndian: bool = false) -> Bytes
fn crc32(data: Bytes) -> int
)";

constexpr std::string_view kCompressQzi = R"(package org.quartzlang.compress

fn deflate(data: Bytes, level: int = 6) -> Bytes
fn inflate(data: Bytes) -> Result<Bytes, str>
fn zlibCompress(data: Bytes, level: int = 6) -> Bytes
fn zlibDecompress(data: Bytes) -> Result<Bytes, str>
fn gzip(data: Bytes, level: int = 6) -> Bytes
fn gunzip(data: Bytes) -> Result<Bytes, str>
fn zstdCompress(data: Bytes, level: int = 3) -> Bytes
fn zstdDecompress(data: Bytes) -> Result<Bytes, str>
)";

constexpr std::string_view kZipQz = R"(package org.quartzlang.zip

pub struct ZipEntry {
    name: str
    data: Bytes
}
)";

constexpr std::string_view kZipQzi = R"(package org.quartzlang.zip

fn readZip(data: Bytes) -> Result<[ZipEntry], str>
fn writeZip(entries: [ZipEntry], level: int = 6) -> Bytes
)";

// The free math functions are built into the compiler; the constants are the doubles nearest to each value.
constexpr std::string_view kMathQz = R"(package org.quartzlang.math

pub const PI: float = 3.141592653589793
pub const TAU: float = 6.283185307179586
pub const INFINITY: float = 1.0 / 0.0
pub const MAX_INT: int = 9223372036854775807
pub const MIN_INT: int = -9223372036854775807 - 1
)";

// xoroshiro128++ seeded through SplitMix64, in plain integer code: every platform and tier gives the same sequence.
constexpr std::string_view kRandomQz = R"(package org.quartzlang.random

/// A seeded generator; copies share one sequence. `words` is its internal state, filled on first use.
pub struct Random {
    seed: int
    words: [int] = []
}

fn rotl(x: int, k: int) -> int {
    return (x << k) | ((x as u64) >> (64 - k)) as int
}

fn splitMix(x: int) -> int {
    var z = x
    z = (z ^ ((z as u64) >> 30) as int) * -4658895280553007687
    z = (z ^ ((z as u64) >> 27) as int) * -7723592293110705685
    return z ^ ((z as u64) >> 31) as int
}

impl Random {
    /// The next 64 random bits.
    pub fn nextLong(self) -> int {
        let s = self.words
        if s.len() != 2 {
            s.clear()
            s.push(splitMix(self.seed + -7046029254386353131))
            s.push(splitMix(self.seed + -7046029254386353131 * 2))
        }
        let s0 = s[0]
        var s1 = s[1]
        let result = rotl(s0 + s1, 17) + s0
        s1 ^= s0
        s[0] = rotl(s0, 49) ^ s1 ^ (s1 << 21)
        s[1] = rotl(s1, 28)
        return result
    }

    /// Uniform in [0, bound) without modulo bias; bound must be positive.
    pub fn nextInt(self, bound: int) -> int {
        if bound <= 0 {
            panic("nextInt bound must be positive, got {bound}")
        }
        let b = bound as u64
        let threshold = (0 as u64 - b) % b
        while true {
            let r = self.nextLong() as u64
            if r >= threshold {
                return (r % b) as int
            }
        }
        return 0
    }

    /// Uniform in [from, until); until must be greater than from.
    pub fn nextIntIn(self, from: int, until: int) -> int {
        if until <= from {
            panic("nextIntIn needs from < until, got {from} and {until}")
        }
        let span = (until - from) as u64
        let threshold = (0 as u64 - span) % span
        while true {
            let r = self.nextLong() as u64
            if r >= threshold {
                return from + (r % span) as int
            }
        }
        return 0
    }

    /// Uniform in [0, 1), a multiple of 2^-53.
    pub fn nextFloat(self) -> float {
        return ((self.nextLong() as u64) >> 11) as float * 1.1102230246251565e-16
    }

    pub fn nextBool(self) -> bool {
        return self.nextLong() < 0
    }

    /// Fisher-Yates from the last element down.
    pub fn shuffle<T>(self, items: [T]) {
        var i = items.len() - 1
        while i > 0 {
            let j = self.nextInt(i + 1)
            let t = items[i]
            items[i] = items[j]
            items[j] = t
            i -= 1
        }
    }
}
)";

// The higher order list methods (`xs.map(f)`): sema calls these with the list as the first argument.
constexpr std::string_view kListsQz = R"(package org.quartzlang.lists

import org.quartzlang.math.min

pub fn map<T, U>(xs: [T], f: fn(T) -> U) -> [U] {
    let out = List::<U>()
    for x in xs {
        out.push(f(x))
    }
    return out
}

pub fn filter<T>(xs: [T], keep: fn(T) -> bool) -> [T] {
    let out = List::<T>()
    for x in xs {
        if keep(x) {
            out.push(x)
        }
    }
    return out
}

pub fn any<T>(xs: [T], test: fn(T) -> bool) -> bool {
    for x in xs {
        if test(x) {
            return true
        }
    }
    return false
}

pub fn all<T>(xs: [T], test: fn(T) -> bool) -> bool {
    for x in xs {
        if !test(x) {
            return false
        }
    }
    return true
}

pub fn find<T>(xs: [T], test: fn(T) -> bool) -> T? {
    for x in xs {
        if test(x) {
            return x
        }
    }
    return null
}

pub fn indexWhere<T>(xs: [T], test: fn(T) -> bool) -> int? {
    for i, x in xs {
        if test(x) {
            return i
        }
    }
    return null
}

/// Stable; each key is computed once, and the keys are compared like sort() does.
pub fn sortBy<T, K>(xs: [T], key: fn(T) -> K) {
    let keys = List::<K>()
    for x in xs {
        keys.push(key(x))
    }
    xs.sortByKeys(keys)
}

/// Stable merge sort; cmp(a, b) < 0 puts a first, and equal elements keep their order.
pub fn sortWith<T>(xs: [T], cmp: fn(T, T) -> int) {
    let n = xs.len()
    if n < 2 {
        return
    }
    var src = xs.slice(0, n)
    var dst = xs.slice(0, n)
    var width = 1
    while width < n {
        var lo = 0
        while lo < n {
            let mid = min(lo + width, n)
            let hi = min(lo + 2 * width, n)
            var i = lo
            var j = mid
            var k = lo
            while k < hi {
                if i < mid && (j >= hi || cmp(src[j], src[i]) >= 0) {
                    dst[k] = src[i]
                    i += 1
                } else {
                    dst[k] = src[j]
                    j += 1
                }
                k += 1
            }
            lo = hi
        }
        let t = src
        src = dst
        dst = t
        width *= 2
    }
    for k in 0..n {
        xs[k] = src[k]
    }
}
)";

} // namespace

const std::vector<std::pair<std::string_view, std::string_view>>& sources() {
    static const std::vector<std::pair<std::string_view, std::string_view>> kSources = {
        {"<std>/data.qz", kDataQz},
        {"<std>/data.qzi", kDataQzi},
        {"<std>/bytes.qzi", kBytesQzi},
        {"<std>/compress.qzi", kCompressQzi},
        {"<std>/zip.qz", kZipQz},
        {"<std>/zip.qzi", kZipQzi},
        {"<std>/math.qz", kMathQz},
        {"<std>/random.qz", kRandomQz},
        {"<std>/lists.qz", kListsQz},
    };
    static const auto kAll = [] {
        auto all = kSources;
        addLibrarySources(all);
        return all;
    }();
    return kAll;
}

} // namespace quartz::stdlib
