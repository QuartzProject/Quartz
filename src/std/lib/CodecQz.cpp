#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kCodecQz = R"qz(package org.quartzlang.data

/// Why a value could not be decoded: where (`servers[2].port`), what was expected and what was there instead.
pub struct DecodeError {
    path: str
    expected: str
    found: str
    /// The line in the source text, or 0 when it is not known.
    line: int = 0
}

impl DecodeError {
    /// `line 3: servers[2].port: expected int, found str "x"`; for text that does not parse, the parser's message.
    pub fn message(self) -> str {
        if self.expected.len() == 0 {
            return self.found
        }
        let where = if self.line > 0 { "line {self.line}: " } else { "" }
        let path = if self.path.len() > 0 { "{self.path}: " } else { "" }
        return "{where}{path}expected {self.expected}, found {self.found}"
    }
}

impl Show for DecodeError {
    fn show(self) -> str {
        return self.message()
    }
}

fn bareKey(key: str) -> bool {
    if key.len() == 0 {
        return false
    }
    for i in 0..key.len() {
        let b = key.byteAt(i)
        if !((b >= 97 && b <= 122) || (b >= 65 && b <= 90) || (b >= 48 && b <= 57) || b == 95 || b == 45) {
            return false
        }
    }
    return true
}

// Keys that are not bare go in brackets, quoted, so the path reads back unambiguously.
fn keySegment(key: str) -> str {
    if bareKey(key) {
        return key
    }
    return "[\"" + key.replace("\\", "\\\\").replace("\"", "\\\"") + "\"]"
}

fn joinPath(head: str, rest: str) -> str {
    if rest.len() == 0 {
        return head
    }
    return if rest.startsWith("[") { head + rest } else { head + "." + rest }
}

fn describe(v: Value) -> str {
    return match v {
        Null => "null"
        Bool(b) => "bool {b}"
        Int(i) => "int {i}"
        Float(f) => "float {f}"
        Str(s) => "str \"{shorten(s)}\""
        List(items) => "a list of {items.len()}"
        Table(_) => "a table"
    }
}

fn shorten(s: str) -> str {
    let cs = s.chars()
    if cs.len() <= 40 {
        return s
    }
    var out = ""
    for i in 0..37 {
        out += "{cs[i]}"
    }
    return out + "..."
}

fn decodeMismatch(expected: str, found: Value) -> DecodeError {
    return DecodeError("", expected, describe(found))
}

fn decodeMissing(key: str, expected: str) -> DecodeError {
    return DecodeError(keySegment(key), expected, "nothing")
}

fn decodeAtKey(e: DecodeError, key: str) -> DecodeError {
    return DecodeError(joinPath(keySegment(key), e.path), e.expected, e.found, e.line)
}

fn decodeAtIndex(e: DecodeError, index: int) -> DecodeError {
    return DecodeError(joinPath("[{index}]", e.path), e.expected, e.found, e.line)
}

/// Converts a `Value` tree to `T`: a type that derives `Decode`, or a scalar, `str`, list, nullable or `Map<str, _>`
/// of such types.
pub fn decode<T: Decode>(value: Value) -> Result<T, DecodeError> {
    return decodeAs::<T>(value)
}

/// The `Value` tree of `x`: struct fields in declaration order (null ones left out), enums by variant name.
pub fn encode<T: Encode>(x: T) -> Value {
    return encodeAs(x)
}

// Decodes parsed text; on an error the path is looked up again in the text for its line.
fn decodeText<T: Decode>(parsed: Result<Value, str>, text: str, format: int) -> Result<T, DecodeError> {
    var value = Value.Null
    match parsed {
        Ok(v) => {
            value = v
        }
        Err(message) => {
            return Err(DecodeError("", "", message))
        }
    }
    return match decodeAs::<T>(value) {
        Ok(x) => Ok(x)
        Err(e) => Err(DecodeError(e.path, e.expected, e.found, lineOf(text, format, e.path)))
    }
}

/// Parses TOML and decodes it to `T`; errors name the path, the expected type, what was found and the line.
pub fn decodeToml<T: Decode>(text: str) -> Result<T, DecodeError> {
    return decodeText::<T>(parseToml(text), text, 0)
}

pub fn decodeJson<T: Decode>(text: str) -> Result<T, DecodeError> {
    return decodeText::<T>(parseJson(text), text, 1)
}

pub fn decodeYaml<T: Decode>(text: str) -> Result<T, DecodeError> {
    return decodeText::<T>(parseYaml(text), text, 2)
}

/// `x` as JSON, compact or indented with `indent` spaces; keys keep the struct's field order.
pub fn encodeJson<T: Encode>(x: T, indent: int = 0) -> str {
    return toJsonOrdered(encodeAs(x), indent)
}

/// `x` as a TOML document; an error if it is not a table or holds a null TOML cannot write.
pub fn encodeToml<T: Encode>(x: T) -> Result<str, str> {
    return toToml(encodeAs(x))
}
)qz";

const std::string_view kCodecQzi = R"qz(package org.quartzlang.data

/// Like `toJson`, with the keys of each table in insertion order instead of sorted.
fn toJsonOrdered(value: Value, indent: int = 0) -> str
/// A table as a TOML document: plain keys first, then `[tables]` and `[[arrays of tables]]`, in insertion order.
fn toToml(value: Value) -> Result<str, str>
fn lineOf(text: str, format: int, path: str) -> int
)qz";

} // namespace quartz::stdlib
