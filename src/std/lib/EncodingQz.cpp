#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kEncodingQzi = R"qz(package org.quartzlang.encoding

/// RFC 4648 base64; `urlSafe` uses `-` and `_` instead of `+` and `/`.
fn base64Encode(data: Bytes, urlSafe: bool = false, padding: bool = true) -> str
/// Accepts text with or without padding; any other character, or non-zero bits after the last byte, is an error.
fn base64Decode(text: str, urlSafe: bool = false) -> Result<Bytes, str>
/// Two digits per byte, upper or lower case; `data.hex()` is the other direction.
fn hexDecode(text: str) -> Result<Bytes, str>
)qz";

const std::string_view kEncodingQz = R"qz(package org.quartzlang.encoding

import org.quartzlang.bytes.{bytesOf, concatBytes, readUInt, writeUInt}
import org.quartzlang.crypto.sha1

/// A 128-bit UUID (RFC 9562) as two big-endian halves.
@derive(Eq, Hash)
pub struct Uuid {
    high: int
    low: int
}

const DnsHigh: int = 0x6ba7b8109dad11d1
const UrlHigh: int = 0x6ba7b8119dad11d1
const NamespaceLow: u64 = 0x80b400c04fd430c8

/// The namespaces of RFC 9562 appendix A for `uuidV5`.
pub const NAMESPACE_DNS: Uuid = Uuid(DnsHigh, NamespaceLow as int)
pub const NAMESPACE_URL: Uuid = Uuid(UrlHigh, NamespaceLow as int)
pub const NIL_UUID: Uuid = Uuid(0, 0)

impl Show for Uuid {
    fn show(self) -> str {
        return self.toStr()
    }
}

impl Ord for Uuid {
    fn cmp(self, other: Uuid) -> int {
        return self.compareTo(other)
    }
}

impl Uuid {
    pub fn version(self) -> int {
        return (self.high >> 12) & 15
    }

    /// The 16 bytes, most significant first.
    pub fn toBytes(self) -> Bytes {
        return writeUInt(writeUInt(bytesOf([]), 0, 8, self.high, true), 8, 8, self.low, true)
    }

    /// Lower case, 8-4-4-4-12 digits.
    pub fn toStr(self) -> str {
        let h = self.toBytes().hex()
        return "{h.slice(0, 8)}-{h.slice(8, 12)}-{h.slice(12, 16)}-{h.slice(16, 20)}-{h.slice(20, 32)}"
    }

    pub fn equals(self, other: Uuid) -> bool {
        return self.high == other.high && self.low == other.low
    }

    /// Orders like the text form: as unsigned 128-bit numbers.
    pub fn compareTo(self, other: Uuid) -> int {
        let a = self.high as u64
        let b = other.high as u64
        if a != b {
            return if a < b { -1 } else { 1 }
        }
        let c = self.low as u64
        let d = other.low as u64
        return if c < d { -1 } else if c > d { 1 } else { 0 }
    }
}

fn withVersion(bytes: Bytes, version: int) -> Uuid {
    let high = readUInt(bytes, 0, 8, true)
    let low = readUInt(bytes, 8, 8, true)
    return Uuid((high & ~0xf000) | (version << 12), (low & 0x3fffffffffffffff) | (1 << 63))
}

fn hexValue(c: int) -> bool {
    return (c >= 48 && c <= 57) || (c >= 97 && c <= 102) || (c >= 65 && c <= 70)
}

/// Reads the 8-4-4-4-12 form in either case.
pub fn parseUuid(text: str) -> Result<Uuid, str> {
    if text.len() != 36 {
        return Err("a UUID has 36 characters, got {text.len()}")
    }
    for i in 0..36 {
        let c = text.byteAt(i)
        let dash = i == 8 || i == 13 || i == 18 || i == 23
        if (dash && c != 45) || (!dash && !hexValue(c)) {
            return Err("bad UUID character at {i}")
        }
    }
    let digits = "{text.slice(0, 8)}{text.slice(9, 13)}{text.slice(14, 18)}{text.slice(19, 23)}{text.slice(24, 36)}"
    let bytes = hexDecode(digits)?
    return Ok(Uuid(readUInt(bytes, 0, 8, true), readUInt(bytes, 8, 8, true)))
}

pub fn uuidFromBytes(data: Bytes) -> Result<Uuid, str> {
    if data.len() != 16 {
        return Err("a UUID has 16 bytes, got {data.len()}")
    }
    return Ok(Uuid(readUInt(data, 0, 8, true), readUInt(data, 8, 8, true)))
}

/// A random UUID from 16 bytes of entropy the caller supplies (the host's random source, say).
pub fn uuidV4(random: Bytes) -> Uuid {
    if random.len() != 16 {
        panic("uuidV4 needs 16 random bytes, got {random.len()}")
    }
    return withVersion(random, 4)
}

/// The name-based UUID of `name` in `namespace`, with SHA-1.
pub fn uuidV5(namespace: Uuid, name: str) -> Uuid {
    return withVersion(sha1(concatBytes([namespace.toBytes(), name.toBytes()])), 5)
}
)qz";

} // namespace quartz::stdlib
