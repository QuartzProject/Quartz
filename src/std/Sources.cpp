#include "std/Std.hpp"

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
    };
    return kSources;
}

} // namespace quartz::stdlib
