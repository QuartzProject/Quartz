#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kBitsQzi = R"qz(package org.quartzlang.bytes

/// The IEEE 754 bits of a float, and back (every NaN reads as Quartz's one NaN).
fn floatToBits(x: float) -> int
fn floatFromBits(bits: int) -> float
/// The same for single precision: `x` is rounded to the nearest f32 first.
fn f32ToBits(x: float) -> int
fn f32FromBits(bits: int) -> float
)qz";

const std::string_view kBytesQz = R"qz(package org.quartzlang.bytes

/// Reads values from `data` at a cursor that moves past them; little-endian unless `bigEndian`. Copies share the cursor.
/// Reading past the end is an error, and the cursor then stays where it was.
pub struct ByteReader {
    data: Bytes
    bigEndian: bool = false
    cursor: [int] = [0]
}

fn short(r: ByteReader, n: int) -> str {
    return "end of data: {n} bytes needed at {r.cursor[0]} of {r.data.len()}"
}

fn signExtend(v: int, bits: int) -> int {
    return (v << (64 - bits)) >> (64 - bits)
}

impl ByteReader {
    pub fn position(self) -> int {
        return self.cursor[0]
    }

    pub fn remaining(self) -> int {
        return self.data.len() - self.cursor[0]
    }

    /// Moves the cursor; a position outside 0 to `data.len()` is a panic.
    pub fn seek(self, position: int) {
        if position < 0 || position > self.data.len() {
            panic("seek to {position} of {self.data.len()} bytes")
        }
        self.cursor[0] = position
    }

    fn unsigned(self, n: int) -> Result<int, str> {
        let p = self.cursor[0]
        if p + n > self.data.len() {
            return Err(short(self, n))
        }
        self.cursor[0] = p + n
        return Ok(readUInt(self.data, p, n, self.bigEndian))
    }

    pub fn readU8(self) -> Result<int, str> {
        return self.unsigned(1)
    }

    pub fn readI8(self) -> Result<int, str> {
        return Ok(signExtend(self.unsigned(1)?, 8))
    }

    pub fn readU16(self) -> Result<int, str> {
        return self.unsigned(2)
    }

    pub fn readI16(self) -> Result<int, str> {
        return Ok(signExtend(self.unsigned(2)?, 16))
    }

    pub fn readU32(self) -> Result<int, str> {
        return self.unsigned(4)
    }

    pub fn readI32(self) -> Result<int, str> {
        return Ok(signExtend(self.unsigned(4)?, 32))
    }

    /// Eight bytes as `int`; an unsigned value above MAX_INT reads as negative (`as u64` gives it back).
    pub fn readI64(self) -> Result<int, str> {
        return self.unsigned(8)
    }

    pub fn readF32(self) -> Result<float, str> {
        return Ok(f32FromBits(self.unsigned(4)?))
    }

    pub fn readF64(self) -> Result<float, str> {
        return Ok(floatFromBits(self.unsigned(8)?))
    }

    pub fn readBytes(self, n: int) -> Result<Bytes, str> {
        let p = self.cursor[0]
        if n < 0 || p + n > self.data.len() {
            return Err(short(self, n))
        }
        self.cursor[0] = p + n
        return Ok(self.data.slice(p, p + n))
    }

    /// `n` bytes of UTF-8 text.
    pub fn readStr(self, n: int) -> Result<str, str> {
        let p = self.cursor[0]
        let b = self.readBytes(n)?
        if let s = b.toStr() {
            return Ok(s)
        }
        self.cursor[0] = p
        return Err("bytes at {p} are not UTF-8 text")
    }

    /// Unsigned LEB128, at most 10 bytes; values past 63 bits read as negative.
    pub fn readVarUInt(self) -> Result<int, str> {
        let p = self.cursor[0]
        var value = 0
        var shift = 0
        while true {
            if self.cursor[0] >= self.data.len() {
                self.cursor[0] = p
                return Err("end of data in a varint at {p}")
            }
            let b = self.data.get(self.cursor[0])
            self.cursor[0] += 1
            if shift == 63 && b > 1 {
                self.cursor[0] = p
                return Err("varint at {p} is longer than 64 bits")
            }
            value |= (b & 127) << shift
            if b < 128 {
                return Ok(value)
            }
            shift += 7
        }
        return Ok(value)
    }

    /// Zigzag then LEB128, as protobuf's sint64: small negative numbers stay short.
    pub fn readVarInt(self) -> Result<int, str> {
        let z = self.readVarUInt()?
        return Ok(((z as u64) >> 1) as int ^ -(z & 1))
    }

    /// A 32-bit two's complement value in at most 5 LEB128 bytes, sign-extended.
    pub fn readVarInt32(self) -> Result<int, str> {
        let p = self.cursor[0]
        let v = self.readVarUInt()?
        if self.cursor[0] - p > 5 || v > 4294967295 {
            self.cursor[0] = p
            return Err("varint at {p} is longer than 32 bits")
        }
        return Ok(signExtend(v, 32))
    }
}

/// Builds bytes: little-endian unless `bigEndian`; integers keep their low bits. Copies share the buffer.
pub struct ByteWriter {
    bigEndian: bool = false
    buffer: [int] = []
}

impl ByteWriter {
    pub fn len(self) -> int {
        return self.buffer.len()
    }

    pub fn toBytes(self) -> Bytes {
        return bytesOf(self.buffer)
    }

    fn put(self, v: int, n: int) {
        for k in 0..n {
            let shift = if self.bigEndian { 8 * (n - 1 - k) } else { 8 * k }
            self.buffer.push(((v as u64) >> shift) as int & 255)
        }
    }

    pub fn writeU8(self, v: int) {
        self.buffer.push(v & 255)
    }

    pub fn writeU16(self, v: int) {
        self.put(v, 2)
    }

    pub fn writeU32(self, v: int) {
        self.put(v, 4)
    }

    pub fn writeI64(self, v: int) {
        self.put(v, 8)
    }

    pub fn writeF32(self, v: float) {
        self.put(f32ToBits(v), 4)
    }

    pub fn writeF64(self, v: float) {
        self.put(floatToBits(v), 8)
    }

    pub fn writeBytes(self, data: Bytes) {
        for b in data.toList() {
            self.buffer.push(b)
        }
    }

    /// The UTF-8 bytes, without a length or terminator.
    pub fn writeStr(self, text: str) {
        self.writeBytes(text.toBytes())
    }

    pub fn writeVarUInt(self, v: int) {
        var rest = v as u64
        while rest >= 128 {
            self.buffer.push((rest & 127) as int | 128)
            rest = rest >> 7
        }
        self.buffer.push(rest as int)
    }

    pub fn writeVarInt(self, v: int) {
        self.writeVarUInt((v << 1) ^ (v >> 63))
    }

    /// The low 32 bits as two's complement, 1 to 5 bytes.
    pub fn writeVarInt32(self, v: int) {
        self.writeVarUInt(v & 4294967295)
    }
}
)qz";

} // namespace quartz::stdlib
