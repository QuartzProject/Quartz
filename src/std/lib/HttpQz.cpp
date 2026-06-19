#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kHttpQzi = R"qz(package org.quartzlang.http

import org.quartzlang.data.Value

/// Percent-encodes every byte but the RFC 3986 unreserved characters (letters, digits, `-._~`).
fn percentEncode(text: str) -> str
fn percentDecode(text: str) -> Result<str, str>
/// `application/x-www-form-urlencoded`, as browsers write it: `a=1&b=x+y`.
fn encodeQuery(params: [(str, str)]) -> str
fn decodeQuery(query: str) -> Result<[(str, str)], str>
/// Runs a request through the host. Quartz has no network of its own: a host that grants HTTP binds this name; any
/// other host answers with an error.
fn send(request: Request) -> Result<Response, str>
)qz";

const std::string_view kHttpQz = R"qz(package org.quartzlang.http

import org.quartzlang.data.{Value, parseJson}

/// An absolute URL split as RFC 3986 does; the parts keep their percent escapes. `host` is null without an authority
/// (`mailto:x`) and empty for an empty one (`file:///etc`).
pub struct Url {
    scheme: str
    userInfo: str?
    host: str?
    port: int?
    path: str
    query: str?
    fragment: str?
}

fn isAlpha(c: int) -> bool {
    return (c >= 65 && c <= 90) || (c >= 97 && c <= 122)
}

fn isDigit(c: int) -> bool {
    return c >= 48 && c <= 57
}

/// `scheme:[//[user@]host[:port]]path[?query][#fragment]`; scheme and host are lowered. Text outside printable ASCII
/// and bad `%` escapes are errors.
pub fn parseUrl(text: str) -> Result<Url, str> {
    for i in 0..text.len() {
        let c = text.byteAt(i)
        if c <= 32 || c >= 127 {
            return Err("URL character at {i} must be percent-encoded")
        }
        if c == 37 && (i + 2 >= text.len() || percentDecode(text.slice(i, i + 3)).isErr()) {
            return Err("bad percent escape at {i}")
        }
    }
    let colon = text.find(":") ?? -1
    if colon < 1 || !isAlpha(text.byteAt(0)) {
        return Err("'{text}' has no scheme")
    }
    for i in 1..colon {
        let c = text.byteAt(i)
        if !isAlpha(c) && !isDigit(c) && c != 43 && c != 45 && c != 46 {
            return Err("bad scheme in '{text}'")
        }
    }
    var rest = text.slice(colon + 1, text.len())
    var fragment: str? = null
    if let hash = rest.find("#") {
        fragment = rest.slice(hash + 1, rest.len())
        rest = rest.slice(0, hash)
    }
    var query: str? = null
    if let q = rest.find("?") {
        query = rest.slice(q + 1, rest.len())
        rest = rest.slice(0, q)
    }
    var userInfo: str? = null
    var host: str? = null
    var port: int? = null
    var path = rest
    if rest.startsWith("//") {
        let slash = rest.slice(2, rest.len()).find("/") ?? (rest.len() - 2)
        var authority = rest.slice(2, slash + 2)
        path = rest.slice(slash + 2, rest.len())
        if let sign = authority.find("@") {
            userInfo = authority.slice(0, sign)
            authority = authority.slice(sign + 1, authority.len())
        }
        var portFrom = -1
        if authority.startsWith("[") {
            let close = authority.find("]") ?? -1
            if close < 0 {
                return Err("unclosed '[' in the host of '{text}'")
            }
            host = authority.slice(0, close + 1).lower()
            if close + 1 < authority.len() {
                if authority.byteAt(close + 1) != 58 {
                    return Err("bad host in '{text}'")
                }
                portFrom = close + 2
            }
        } else {
            let pc = authority.find(":") ?? -1
            host = (if pc >= 0 { authority.slice(0, pc) } else { authority }).lower()
            portFrom = if pc >= 0 { pc + 1 } else { -1 }
        }
        if portFrom >= 0 {
            let digits = authority.slice(portFrom, authority.len())
            if digits.len() > 0 {
                let p = digits.toInt() ?? -1
                if p < 0 || p > 65535 || digits.startsWith("+") || digits.startsWith("-") {
                    return Err("bad port in '{text}'")
                }
                port = p
            }
        }
    }
    return Ok(Url(text.slice(0, colon).lower(), userInfo, host, port, path, query, fragment))
}

impl Url {
    pub fn parse(text: str) -> Result<Url, str> {
        return parseUrl(text)
    }

    pub fn toStr(self) -> str {
        var out = self.scheme + ":"
        if let h = self.host {
            out = out + "//"
            if let u = self.userInfo {
                out = out + u + "@"
            }
            out = out + h
            if let p = self.port {
                out = out + ":{p}"
            }
        }
        out = out + self.path
        if let q = self.query {
            out = out + "?" + q
        }
        if let f = self.fragment {
            out = out + "#" + f
        }
        return out
    }

    /// The decoded `key=value` pairs of the query, in order.
    pub fn queryParams(self) -> Result<[(str, str)], str> {
        return decodeQuery(self.query ?? "")
    }

    pub fn withQuery(self, params: [(str, str)]) -> Url {
        return Url(self.scheme, self.userInfo, self.host, self.port, self.path, encodeQuery(params), self.fragment)
    }
}

pub struct Request {
    url: str
    method: str = "GET"
    headers: [(str, str)] = []
    body: Bytes? = null
    timeoutMillis: int = 10000
}

impl Request {
    /// A copy with one more header.
    pub fn withHeader(self, name: str, value: str) -> Request {
        let hs = self.headers.slice(0, self.headers.len())
        hs.push((name, value))
        return Request(self.url, self.method, hs, self.body, self.timeoutMillis)
    }
}

pub struct Response {
    status: int
    headers: [(str, str)] = []
    body: Bytes
}

impl Response {
    pub fn isSuccess(self) -> bool {
        return self.status >= 200 && self.status < 300
    }

    /// The first header with this name, ignoring ASCII case.
    pub fn header(self, name: str) -> str? {
        for h in self.headers {
            if h.0.equalsIgnoreCase(name) {
                return h.1
            }
        }
        return null
    }

    /// The body as text, or null when it is not UTF-8.
    pub fn text(self) -> str? {
        return self.body.toStr()
    }

    pub fn json(self) -> Result<Value, str> {
        if let t = self.body.toStr() {
            return parseJson(t)
        }
        return Err("the body is not UTF-8 text")
    }
}
)qz";

const std::string_view kLogQzi = R"qz(package org.quartzlang.log

/// The host's log sink: `level` 0 to 4 (trace to error), `fields` as key, value, key, value. Records are dropped unless
/// the host binds this name.
fn emit(level: int, logger: str, message: str, fields: [str])
)qz";

const std::string_view kLogQz = R"qz(package org.quartzlang.log

pub enum Level {
    Trace
    Debug
    Info
    Warn
    Error
}

fn rank(level: Level) -> int {
    return match level {
        Trace => 0
        Debug => 1
        Info => 2
        Warn => 3
        Error => 4
    }
}

/// A named logger with structured fields; `with` and `atLevel` return changed copies.
pub struct Logger {
    name: str
    fields: [str] = []
    minimum: int = 2
}

pub fn logger(name: str) -> Logger {
    return Logger(name)
}

impl Logger {
    pub fn with(self, key: str, value: str) -> Logger {
        let fs = self.fields.slice(0, self.fields.len())
        fs.push(key)
        fs.push(value)
        return Logger(self.name, fs, self.minimum)
    }

    /// Drops records below `level`; the default is Info.
    pub fn atLevel(self, level: Level) -> Logger {
        return Logger(self.name, self.fields, rank(level))
    }

    pub fn isEnabled(self, level: Level) -> bool {
        return rank(level) >= self.minimum
    }

    pub fn log(self, level: Level, message: str) {
        if rank(level) >= self.minimum {
            emit(rank(level), self.name, message, self.fields)
        }
    }

    pub fn trace(self, message: str) {
        self.log(Level.Trace, message)
    }

    pub fn debug(self, message: str) {
        self.log(Level.Debug, message)
    }

    pub fn info(self, message: str) {
        self.log(Level.Info, message)
    }

    pub fn warn(self, message: str) {
        self.log(Level.Warn, message)
    }

    pub fn error(self, message: str) {
        self.log(Level.Error, message)
    }
}
)qz";

} // namespace quartz::stdlib
