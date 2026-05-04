#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kTimeQz = R"qz(package org.quartzlang.time

const NanosPerMicro: int = 1000
const NanosPerMilli: int = 1000000
const NanosPerSecond: int = 1000000000
const NanosPerMinute: int = 60000000000
const NanosPerHour: int = 3600000000000
const NanosPerDay: int = 86400000000000
const NanosPerWeek: int = 604800000000000

fn must(v: int?, what: str) -> int {
    if let x = v {
        return x
    }
    panic("{what} is out of range (about 292 years either way)")
}

fn floorDiv(a: int, b: int) -> int {
    let q = a / b
    return if (a % b != 0) && ((a < 0) != (b < 0)) { q - 1 } else { q }
}

fn floorMod(a: int, b: int) -> int {
    return a - floorDiv(a, b) * b
}

/// A signed span of time in nanoseconds, up to about 292 years either way; overflowing it is a panic.
pub struct Duration {
    nanos: int
}

pub fn nanos(n: int) -> Duration {
    return Duration(n)
}

pub fn micros(n: int) -> Duration {
    return Duration(must(n.checkedMul(NanosPerMicro), "duration"))
}

pub fn millis(n: int) -> Duration {
    return Duration(must(n.checkedMul(NanosPerMilli), "duration"))
}

pub fn seconds(n: int) -> Duration {
    return Duration(must(n.checkedMul(NanosPerSecond), "duration"))
}

pub fn minutes(n: int) -> Duration {
    return Duration(must(n.checkedMul(NanosPerMinute), "duration"))
}

pub fn hours(n: int) -> Duration {
    return Duration(must(n.checkedMul(NanosPerHour), "duration"))
}

pub fn days(n: int) -> Duration {
    return Duration(must(n.checkedMul(NanosPerDay), "duration"))
}

fn unitNanos(unit: str) -> int {
    return match unit {
        "ns" => 1
        "us" => NanosPerMicro
        "µs" => NanosPerMicro
        "μs" => NanosPerMicro
        "ms" => NanosPerMilli
        "s" => NanosPerSecond
        "m" => NanosPerMinute
        "h" => NanosPerHour
        "d" => NanosPerDay
        "w" => NanosPerWeek
        _ => 0
    }
}

fn isDigit(c: int) -> bool {
    return c >= 48 && c <= 57
}

/// Parts like `1h30m`, `1.5s`, `250ms`, `2d 4h` or `-90s`: units ns, us (or µs), ms, s, m, h, d and w; `0` alone too.
pub fn parseDuration(text: str) -> Result<Duration, str> {
    let n = text.len()
    var i = 0
    var negative = false
    if n > 0 && (text.byteAt(0) == 43 || text.byteAt(0) == 45) {
        negative = text.byteAt(0) == 45
        i = 1
    }
    if text.slice(i, n) == "0" {
        return Ok(Duration(0))
    }
    var total = 0
    var parts = 0
    while i < n {
        if text.byteAt(i) == 32 && parts > 0 {
            i += 1
            continue
        }
        let start = i
        var whole = 0
        while i < n && isDigit(text.byteAt(i)) {
            whole = whole.checkedMul(10)?.checkedAdd(text.byteAt(i) - 48) ?? -1
            if whole < 0 {
                return Err("duration '{text}' is too large")
            }
            i += 1
        }
        let digitsEnd = i
        var fracFrom = i
        var fracTo = i
        if i < n && text.byteAt(i) == 46 {
            i += 1
            fracFrom = i
            while i < n && isDigit(text.byteAt(i)) {
                i += 1
            }
            fracTo = i
        }
        if digitsEnd == start && fracTo == fracFrom {
            return Err("expected a number at {start} in duration '{text}'")
        }
        let unitFrom = i
        while i < n && !isDigit(text.byteAt(i)) && text.byteAt(i) != 32 && text.byteAt(i) != 46 {
            i += 1
        }
        let unit = unitNanos(text.slice(unitFrom, i))
        if unit == 0 {
            return Err("unknown unit '{text.slice(unitFrom, i)}' in duration '{text}' (use ns, us, ms, s, m, h, d or w)")
        }
        var part = whole.checkedMul(unit) ?? -1
        // exact floor of the fraction: carry the remainder over 10^k, digit by digit, up to 18 digits
        var rem = 0
        var scale = 1
        var k = fracFrom
        while k < fracTo && k < fracFrom + 18 && part >= 0 {
            scale *= 10
            let s = rem * 10 + (text.byteAt(k) - 48) * unit
            part = part.checkedAdd(s / scale) ?? -1
            rem = s % scale
            k += 1
        }
        if part < 0 {
            return Err("duration '{text}' is too large")
        }
        total = total.checkedAdd(part) ?? -1
        if total < 0 {
            return Err("duration '{text}' is too large")
        }
        parts += 1
    }
    if parts == 0 {
        return Err("empty duration")
    }
    return Ok(Duration(if negative { -total } else { total }))
}

fn fraction(value: int, digits: int) -> str {
    var s = "{value}"
    while s.len() < digits {
        s = "0" + s
    }
    var end = s.len()
    while end > 0 && s.byteAt(end - 1) == 48 {
        end -= 1
    }
    return s.slice(0, end)
}

fn scaled(n: int, unit: int, digits: int, suffix: str) -> str {
    let rest = n % unit
    if rest == 0 {
        return "{n / unit}{suffix}"
    }
    return "{n / unit}.{fraction(rest, digits)}{suffix}"
}

impl Duration {
    pub fn toNanos(self) -> int {
        return self.nanos
    }

    pub fn toMicros(self) -> int {
        return self.nanos / NanosPerMicro
    }

    pub fn toMillis(self) -> int {
        return self.nanos / NanosPerMilli
    }

    pub fn toSeconds(self) -> int {
        return self.nanos / NanosPerSecond
    }

    pub fn toMinutes(self) -> int {
        return self.nanos / NanosPerMinute
    }

    pub fn toHours(self) -> int {
        return self.nanos / NanosPerHour
    }

    pub fn toDays(self) -> int {
        return self.nanos / NanosPerDay
    }

    pub fn toFloatSeconds(self) -> float {
        return (self.nanos / NanosPerSecond) as float + (self.nanos % NanosPerSecond) as float / 1000000000.0
    }

    pub fn plus(self, other: Duration) -> Duration {
        return Duration(must(self.nanos.checkedAdd(other.nanos), "duration"))
    }

    pub fn minus(self, other: Duration) -> Duration {
        return Duration(must(self.nanos.checkedSub(other.nanos), "duration"))
    }

    pub fn times(self, factor: int) -> Duration {
        return Duration(must(self.nanos.checkedMul(factor), "duration"))
    }

    /// Truncates toward zero; dividing by zero is a trap.
    pub fn div(self, divisor: int) -> Duration {
        return Duration(self.nanos / divisor)
    }

    pub fn negate(self) -> Duration {
        return Duration(must(0.checkedSub(self.nanos), "duration"))
    }

    pub fn abs(self) -> Duration {
        return if self.nanos < 0 { self.negate() } else { self }
    }

    pub fn isZero(self) -> bool {
        return self.nanos == 0
    }

    pub fn isNegative(self) -> bool {
        return self.nanos < 0
    }

    pub fn equals(self, other: Duration) -> bool {
        return self.nanos == other.nanos
    }

    pub fn compareTo(self, other: Duration) -> int {
        return if self.nanos < other.nanos { -1 } else if self.nanos > other.nanos { 1 } else { 0 }
    }

    /// The shortest form `parseDuration` reads back: `1h30m`, `2d5s`, `1.5s`, `250ms`, `1.25us`, `-3m`, `0s`.
    pub fn toStr(self) -> str {
        if self.nanos == 0 {
            return "0s"
        }
        let sign = if self.nanos < 0 { "-" } else { "" }
        let abs = if self.nanos < 0 { (0 as u64 - self.nanos as u64) } else { self.nanos as u64 }
        if abs < NanosPerSecond as u64 {
            let small = abs as int
            if small < NanosPerMicro {
                return "{sign}{small}ns"
            }
            if small < NanosPerMilli {
                return sign + scaled(small, NanosPerMicro, 3, "us")
            }
            return sign + scaled(small, NanosPerMilli, 6, "ms")
        }
        var parts: [str] = [sign]
        var rest = abs
        let units = [(NanosPerDay, "d"), (NanosPerHour, "h"), (NanosPerMinute, "m")]
        for u in units {
            let count = rest / (u.0 as u64)
            if count > 0 {
                parts.push("{count}{u.1}")
                rest -= count * (u.0 as u64)
            }
        }
        if rest > 0 {
            parts.push(scaled(rest as int, NanosPerSecond, 9, "s"))
        }
        return parts.join("")
    }
}

/// A point in time as nanoseconds since 1970-01-01T00:00:00Z without leap seconds (Unix time), from 1677 to 2262.
/// Quartz has no clock: the host hands instants in, through a function of its interface.
pub struct Instant {
    nanos: int
}

pub fn fromEpochNanos(n: int) -> Instant {
    return Instant(n)
}

pub fn fromEpochMillis(ms: int) -> Instant {
    return Instant(must(ms.checkedMul(NanosPerMilli), "instant"))
}

pub fn fromEpochSeconds(s: int) -> Instant {
    return Instant(must(s.checkedMul(NanosPerSecond), "instant"))
}

/// A date and time in UTC; `weekday` is 1 for Monday to 7 for Sunday (ISO 8601).
pub struct DateTime {
    year: int
    month: int
    day: int
    hour: int
    minute: int
    second: int
    nano: int
    weekday: int
}

fn isLeap(y: int) -> bool {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0
}

fn daysInMonth(y: int, m: int) -> int {
    if m == 2 {
        return if isLeap(y) { 29 } else { 28 }
    }
    return if m == 4 || m == 6 || m == 9 || m == 11 { 30 } else { 31 }
}

// Days since 1970-01-01 of a proleptic Gregorian date, over 400-year eras.
fn daysFromCivil(year: int, m: int, d: int) -> int {
    let y = if m <= 2 { year - 1 } else { year }
    let era = floorDiv(y, 400)
    let yoe = y - era * 400
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d - 1
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy
    return era * 146097 + doe - 719468
}

// (year, month, day) of a day count since 1970-01-01.
fn civilFromDays(days: int) -> (int, int, int) {
    let z = days + 719468
    let era = floorDiv(z, 146097)
    let doe = z - era * 146097
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100)
    let mp = (5 * doy + 2) / 153
    let d = doy - (153 * mp + 2) / 5 + 1
    let m = if mp < 10 { mp + 3 } else { mp - 9 }
    return (yoe + era * 400 + (if m <= 2 { 1 } else { 0 }), m, d)
}

fn pad(v: int, width: int) -> str {
    var s = "{v}"
    while s.len() < width {
        s = "0" + s
    }
    return s
}

/// The instant of a UTC date and time; fields out of range are an error.
pub fn instantOf(year: int, month: int, day: int, hour: int = 0, minute: int = 0, second: int = 0, nano: int = 0) -> Result<Instant, str> {
    if month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month) {
        return Err("no such date {year}-{month}-{day}")
    }
    if hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 || nano < 0 || nano >= NanosPerSecond {
        return Err("no such time {hour}:{minute}:{second}.{nano}")
    }
    if year < 1677 || year > 2262 {
        return Err("year {year} is outside 1677 to 2262")
    }
    let secs = daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second
    let n = secs.checkedMul(NanosPerSecond)?.checkedAdd(nano)
    if let v = n {
        return Ok(Instant(v))
    }
    return Err("{year}-{month}-{day} is outside the range of an instant")
}

impl Instant {
    pub fn epochNanos(self) -> int {
        return self.nanos
    }

    /// Rounded down, also before 1970.
    pub fn epochMillis(self) -> int {
        return floorDiv(self.nanos, NanosPerMilli)
    }

    pub fn epochSeconds(self) -> int {
        return floorDiv(self.nanos, NanosPerSecond)
    }

    pub fn plus(self, d: Duration) -> Instant {
        return Instant(must(self.nanos.checkedAdd(d.nanos), "instant"))
    }

    pub fn minus(self, d: Duration) -> Instant {
        return Instant(must(self.nanos.checkedSub(d.nanos), "instant"))
    }

    /// The time from `earlier` to this instant, negative if `earlier` is later.
    pub fn since(self, earlier: Instant) -> Duration {
        return Duration(must(self.nanos.checkedSub(earlier.nanos), "duration"))
    }

    pub fn isBefore(self, other: Instant) -> bool {
        return self.nanos < other.nanos
    }

    pub fn isAfter(self, other: Instant) -> bool {
        return self.nanos > other.nanos
    }

    pub fn equals(self, other: Instant) -> bool {
        return self.nanos == other.nanos
    }

    pub fn compareTo(self, other: Instant) -> int {
        return if self.nanos < other.nanos { -1 } else if self.nanos > other.nanos { 1 } else { 0 }
    }

    pub fn utc(self) -> DateTime {
        let days = floorDiv(self.nanos, NanosPerDay)
        let inDay = self.nanos - days * NanosPerDay
        let ymd = civilFromDays(days)
        let secs = inDay / NanosPerSecond
        return DateTime(ymd.0, ymd.1, ymd.2, secs / 3600, secs / 60 % 60, secs % 60, inDay % NanosPerSecond, floorMod(days + 3, 7) + 1)
    }

    /// RFC 3339 in UTC, with 3, 6 or 9 fraction digits when there is a fraction: `2026-11-19T14:40:00.250Z`.
    pub fn toStr(self) -> str {
        let t = self.utc()
        var frac = ""
        if t.nano != 0 {
            let digits = if t.nano % 1000000 == 0 { 3 } else if t.nano % 1000 == 0 { 6 } else { 9 }
            let value = if digits == 3 { t.nano / 1000000 } else if digits == 6 { t.nano / 1000 } else { t.nano }
            frac = "." + pad(value, digits)
        }
        return "{pad(t.year, 4)}-{pad(t.month, 2)}-{pad(t.day, 2)}T{pad(t.hour, 2)}:{pad(t.minute, 2)}:{pad(t.second, 2)}{frac}Z"
    }
}

impl DateTime {
    pub fn toInstant(self) -> Result<Instant, str> {
        return instantOf(self.year, self.month, self.day, self.hour, self.minute, self.second, self.nano)
    }
}

fn digitsAt(text: str, from: int, count: int) -> int {
    if from + count > text.len() {
        return -1
    }
    var v = 0
    for i in from..from + count {
        let c = text.byteAt(i)
        if !isDigit(c) {
            return -1
        }
        v = v * 10 + c - 48
    }
    return v
}

/// RFC 3339: `2026-11-19T14:40:00Z`, with an optional fraction and `Z` or a `+hh:mm`/`-hh:mm` offset.
pub fn parseInstant(text: str) -> Result<Instant, str> {
    let bad = "'{text}' is not an RFC 3339 date-time"
    let year = digitsAt(text, 0, 4)
    let month = digitsAt(text, 5, 2)
    let day = digitsAt(text, 8, 2)
    let hour = digitsAt(text, 11, 2)
    let minute = digitsAt(text, 14, 2)
    let second = digitsAt(text, 17, 2)
    if year < 0 || month < 0 || day < 0 || hour < 0 || minute < 0 || second < 0 {
        return Err(bad)
    }
    let t = text.byteAt(10)
    if text.byteAt(4) != 45 || text.byteAt(7) != 45 || (t != 84 && t != 116 && t != 32) || text.byteAt(13) != 58 || text.byteAt(16) != 58 {
        return Err(bad)
    }
    var i = 19
    var nano = 0
    if i < text.len() && text.byteAt(i) == 46 {
        i += 1
        var scale = NanosPerSecond
        let from = i
        while i < text.len() && isDigit(text.byteAt(i)) {
            scale /= 10
            nano += (text.byteAt(i) - 48) * scale
            i += 1
        }
        if i == from {
            return Err(bad)
        }
    }
    if i >= text.len() {
        return Err(bad)
    }
    var offset = 0
    let z = text.byteAt(i)
    if z == 90 || z == 122 {
        i += 1
    } else if z == 43 || z == 45 {
        let oh = digitsAt(text, i + 1, 2)
        let om = digitsAt(text, i + 4, 2)
        if oh < 0 || om < 0 || oh > 23 || om > 59 || text.byteAt(i + 3) != 58 {
            return Err(bad)
        }
        offset = (oh * 60 + om) * (if z == 45 { -1 } else { 1 })
        i += 6
    } else {
        return Err(bad)
    }
    if i != text.len() {
        return Err(bad)
    }
    let local = instantOf(year, month, day, hour, minute, second, nano)?
    return Ok(local.minus(minutes(offset)))
}

/// A cron schedule in the classic five-field form (minute, hour, day of month, month, day of week), read like Vixie
/// cron: `*`, lists, ranges, steps, `jan`..`dec`, `sun`..`sat`, Sunday as 0 or 7, and @hourly, @daily, @weekly,
/// @monthly, @yearly. When both day fields are restricted a day matching either one runs; a field starting with `*`
/// counts as unrestricted. No seconds field and no `L`, `W`, `#` or `?`. Times are UTC.
pub struct Cron {
    text: str
    minutes: int
    hours: int
    days: int
    months: int
    weekdays: int
    anyDay: bool
    anyWeekday: bool
}

const MonthNames: str = "jan feb mar apr may jun jul aug sep oct nov dec"
const DayNames: str = "sun mon tue wed thu fri sat"

fn cronValue(word: str, names: str, first: int) -> int {
    if word.len() == 0 {
        return -1
    }
    var v = 0
    for i in 0..word.len() {
        let c = word.byteAt(i)
        if !isDigit(c) {
            if names.len() == 0 || word.len() != 3 {
                return -1
            }
            let pos = names.find(word.lower()) ?? -1
            return if pos >= 0 && pos % 4 == 0 { pos / 4 + first } else { -1 }
        }
        v = v * 10 + c - 48
        if v > 1000 {
            return -1
        }
    }
    return v
}

// The bits of one field, or -1 with the problem in `error[0]`.
fn cronField(field: str, lo: int, hi: int, names: str, what: str, error: [str]) -> int {
    var bits = 0
    for item in field.split(",") {
        var range = item
        var step = 1
        if let slash = item.find("/") {
            step = cronValue(item.slice(slash + 1, item.len()), "", 0)
            range = item.slice(0, slash)
            if step < 1 {
                error.push("bad step in {what} '{item}'")
                return -1
            }
        }
        let first = if what == "month" { 1 } else { 0 }
        var from = lo
        var to = hi
        if range != "*" {
            let dash = range.find("-") ?? -1
            from = cronValue(if dash >= 0 { range.slice(0, dash) } else { range }, names, first)
            to = from
            if dash >= 0 {
                to = cronValue(range.slice(dash + 1, range.len()), names, first)
            } else if range.len() < item.len() {
                to = hi
            }
            if from < lo || to > hi || from > to {
                error.push("bad {what} '{item}' (allowed {lo} to {hi})")
                return -1
            }
        }
        var v = from
        while v <= to {
            bits |= 1 << v
            v += step
        }
    }
    return bits
}

pub fn parseCron(text: str) -> Result<Cron, str> {
    var spec = text.trim()
    let macros = [("@yearly", "0 0 1 1 *"), ("@annually", "0 0 1 1 *"), ("@monthly", "0 0 1 * *"), ("@weekly", "0 0 * * 0"), ("@daily", "0 0 * * *"), ("@midnight", "0 0 * * *"), ("@hourly", "0 * * * *")]
    if spec.startsWith("@") {
        var found = false
        for m in macros {
            if spec.equalsIgnoreCase(m.0) {
                spec = m.1
                found = true
            }
        }
        if !found {
            return Err("unknown cron macro '{spec}'")
        }
    }
    var fields: [str] = []
    for f in spec.replace("\t", " ").split(" ") {
        if f.len() > 0 {
            fields.push(f)
        }
    }
    if fields.len() != 5 {
        return Err("a cron expression has 5 fields (minute hour day month weekday), got {fields.len()}")
    }
    let error: [str] = []
    let mins = cronField(fields[0], 0, 59, "", "minute", error)
    let hrs = cronField(fields[1], 0, 23, "", "hour", error)
    let dom = cronField(fields[2], 1, 31, "", "day of month", error)
    let mon = cronField(fields[3], 1, 12, MonthNames, "month", error)
    var dow = cronField(fields[4], 0, 7, DayNames, "day of week", error)
    if error.len() > 0 {
        return Err(error[0])
    }
    if (dow & (1 << 7)) != 0 {
        dow = (dow | 1) & 127
    }
    return Ok(Cron(text, mins, hrs, dom, mon, dow, fields[2].startsWith("*"), fields[4].startsWith("*")))
}

impl Cron {
    fn dayMatches(self, days: int, day: int) -> bool {
        let inMonth = (self.days & (1 << day)) != 0
        let inWeek = (self.weekdays & (1 << floorMod(days + 4, 7))) != 0
        return if self.anyDay || self.anyWeekday { inMonth && inWeek } else { inMonth || inWeek }
    }

    pub fn matches(self, time: Instant) -> bool {
        let t = time.utc()
        let days = floorDiv(time.nanos, NanosPerDay)
        return (self.months & (1 << t.month)) != 0 && self.dayMatches(days, t.day) && (self.hours & (1 << t.hour)) != 0 && (self.minutes & (1 << t.minute)) != 0
    }

    /// The first matching minute strictly after `after`, or null if there is none within 400 years.
    pub fn next(self, after: Instant) -> Instant? {
        let start = floorDiv(after.nanos, NanosPerMinute) + 1
        var days = floorDiv(start, 1440)
        var minuteOfDay = start - days * 1440
        let firstYear = civilFromDays(days).0
        while true {
            let ymd = civilFromDays(days)
            if ymd.0 > firstYear + 400 || ymd.0 > 2262 {
                return null
            }
            if (self.months & (1 << ymd.1)) == 0 {
                days = if ymd.1 == 12 { daysFromCivil(ymd.0 + 1, 1, 1) } else { daysFromCivil(ymd.0, ymd.1 + 1, 1) }
                minuteOfDay = 0
                continue
            }
            if self.dayMatches(days, ymd.2) {
                var h = minuteOfDay / 60
                var m = minuteOfDay % 60
                while h < 24 {
                    if (self.hours & (1 << h)) != 0 {
                        while m < 60 {
                            if (self.minutes & (1 << m)) != 0 {
                                if let v = (days * 1440 + h * 60 + m).checkedMul(NanosPerMinute) {
                                    return Instant(v)
                                }
                                return null
                            }
                            m += 1
                        }
                    }
                    h += 1
                    m = 0
                }
            }
            days += 1
            minuteOfDay = 0
        }
        return null
    }
}
)qz";

} // namespace quartz::stdlib
