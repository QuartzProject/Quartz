#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kFormatsQzi = R"qz(package org.quartzlang.formats

import org.quartzlang.data.Value

/// RFC 4180 CSV: rows of fields, quoted fields with `""` for a quote; CRLF, LF or CR line ends, a blank line is a row
/// without fields, and a quote inside an unquoted field is kept as text.
fn parseCsv(text: str, separator: str = ",") -> Result<[[str]], str>
/// Quotes only the fields that need it (separator, quote or line break inside, or a lone empty field).
fn toCsv(rows: [[str]], separator: str = ",", crlf: bool = true) -> str
/// A Java `.properties` file: `key=value`, `key: value` or `key value`, `#` and `!` comments, backslash line
/// continuations and the escapes of Properties.load, including `\uXXXX`.
fn parseProperties(text: str) -> Result<Map<str, str>, str>
/// Properties text with the keys sorted, one `key=value` per line, escaped so that `parseProperties` reads it back.
fn toProperties(values: Map<str, str>) -> str
/// MessagePack to a Value: integers past `int` become Float, binary must be UTF-8 text, map keys must be strings;
/// extension types are an error.
fn parseMsgPack(data: Bytes) -> Result<Value, str>
/// The smallest MessagePack encoding of each value, floats as float64, map keys sorted: same value, same bytes.
fn toMsgPack(value: Value) -> Bytes
)qz";

} // namespace quartz::stdlib
