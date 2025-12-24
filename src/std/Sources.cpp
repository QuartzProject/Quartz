#include "std/Std.hpp"

namespace quartz::stdlib {

namespace {

constexpr std::string_view kDataQz = R"(package std.data

pub enum Value {
    Null
    Bool(bool)
    Int(i64)
    Float(f64)
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

    pub fn item(self, index: i64) -> Value? {
        return match self {
            List(items) => if index >= 0 && index < items.len() { items[index] } else { null }
            _ => null
        }
    }

    pub fn len(self) -> i64 {
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

    pub fn asInt(self) -> i64? {
        return match self {
            Int(i) => i
            _ => null
        }
    }

    pub fn asFloat(self) -> f64? {
        return match self {
            Float(f) => f
            Int(i) => i as f64
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

constexpr std::string_view kDataQzi = R"(package std.data

fn parseJson(text: str) -> Result<Value, str>
fn toJson(value: Value, indent: i64 = 0) -> str
fn parseToml(text: str) -> Result<Value, str>
fn parseYaml(text: str) -> Result<Value, str>
)";

constexpr std::string_view kBytesQzi = R"(package std.bytes

impl Bytes {
    fn len(self) -> i64
    fn get(self, index: i64) -> i64
    fn slice(self, begin: i64, end: i64) -> Bytes
    fn toStr(self) -> str?
    fn toList(self) -> [i64]
    fn hex(self) -> str
}

impl str {
    fn toBytes(self) -> Bytes
}

fn bytesOf(values: [i64]) -> Bytes
fn concatBytes(parts: [Bytes]) -> Bytes
fn readUInt(data: Bytes, offset: i64, size: i64, bigEndian: bool = false) -> i64
fn crc32(data: Bytes) -> i64
)";

constexpr std::string_view kCompressQzi = R"(package std.compress

fn deflate(data: Bytes, level: i64 = 6) -> Bytes
fn inflate(data: Bytes) -> Result<Bytes, str>
fn zlibCompress(data: Bytes, level: i64 = 6) -> Bytes
fn zlibDecompress(data: Bytes) -> Result<Bytes, str>
fn gzip(data: Bytes, level: i64 = 6) -> Bytes
fn gunzip(data: Bytes) -> Result<Bytes, str>
)";

constexpr std::string_view kZipQz = R"(package std.zip

pub struct ZipEntry {
    name: str
    data: Bytes
}
)";

constexpr std::string_view kZipQzi = R"(package std.zip

fn readZip(data: Bytes) -> Result<[ZipEntry], str>
fn writeZip(entries: [ZipEntry], level: i64 = 6) -> Bytes
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
    };
    return kSources;
}

} // namespace quartz::stdlib
