#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kRegexQzi = R"qz(package org.quartzlang.regex

/// The names of the groups of `pattern`, the whole match first and "" for a group without a name; the error gives the
/// problem and its byte position. The `regex*` functions are what `Regex` runs on; call them through it.
fn regexGroups(pattern: str) -> Result<[str], str>
fn regexIsMatch(pattern: str, text: str, full: bool) -> bool
/// Start and end of every group of the leftmost match, -1 for a group that took no part; empty without a match.
fn regexFind(pattern: str, text: str) -> [int]
/// The spans of `regexFind` for every match, one after the other.
fn regexFindAll(pattern: str, text: str) -> [int]
fn regexReplace(pattern: str, text: str, replacement: str, all: bool) -> str
fn regexSplit(pattern: str, text: str, limit: int) -> [str]
/// `text` with the regex metacharacters escaped, so the pattern matches exactly `text`.
fn escapeRegex(text: str) -> str
)qz";

const std::string_view kRegexQz = R"qz(package org.quartzlang.regex

/// A regular expression in a subset of RE2 syntax. Matching takes time linear in the text for every pattern: there is
/// no backtracking, so there are no backreferences or lookaround either. Build one with `compileRegex`.
pub struct Regex {
    pattern: str
    names: [str]
}

/// One match; `start` and `end` are byte offsets into the searched text, as `str.slice` takes them.
pub struct Match {
    start: int
    end: int
    text: str
    subject: str
    spans: [int]
    base: int
    names: [str]
}

/// Compiles `pattern`; the error names the problem and its byte position in the pattern.
pub fn compileRegex(pattern: str) -> Result<Regex, str> {
    return match regexGroups(pattern) {
        Ok(names) => Ok(Regex(pattern, names))
        Err(e) => Err(e)
    }
}

fn namedGroups(names: [str]) -> [str] {
    var out: [str] = []
    for i, n in names {
        if i > 0 && n.len() > 0 {
            out.push(n)
        }
    }
    return out
}

fn matchAt(subject: str, spans: [int], base: int, names: [str]) -> Match {
    let start = spans[base]
    let end = spans[base + 1]
    return Match(start, end, subject.slice(start, end), subject, spans, base, names)
}

impl Regex {
    /// True when the pattern matches somewhere in `text`.
    pub fn isMatch(self, text: str) -> bool {
        return regexIsMatch(self.pattern, text, false)
    }

    /// True when the pattern matches the whole of `text`.
    pub fn fullMatch(self, text: str) -> bool {
        return regexIsMatch(self.pattern, text, true)
    }

    /// The leftmost match; among those starting there, earlier alternatives and greedier repeats win, as in Perl.
    pub fn find(self, text: str) -> Match? {
        let spans = regexFind(self.pattern, text)
        if spans.len() == 0 {
            return null
        }
        return matchAt(text, spans, 0, self.names)
    }

    /// Every match from left to right, without overlaps. As in Python, an empty match right after another match
    /// counts, and the search after an empty match moves on one character.
    pub fn findAll(self, text: str) -> [Match] {
        let spans = regexFindAll(self.pattern, text)
        let stride = self.names.len() * 2
        var out: [Match] = []
        var i = 0
        while i + 1 < spans.len() {
            out.push(matchAt(text, spans, i, self.names))
            i += stride
        }
        return out
    }

    /// `text` with the first match replaced. In `replacement`, `$1` or `${1}` is a group by number, `${name}` one by
    /// name, `$0` the whole match and `$$` a dollar; a group that took no part adds nothing. A reference to a group
    /// the pattern does not have panics.
    pub fn replace(self, text: str, replacement: str) -> str {
        return regexReplace(self.pattern, text, replacement, false)
    }

    /// `text` with every match of `findAll` replaced, as `replace` does.
    pub fn replaceAll(self, text: str, replacement: str) -> str {
        return regexReplace(self.pattern, text, replacement, true)
    }

    /// The pieces of `text` between the matches of `findAll`, empty ones included; at most `limit` pieces when it is
    /// positive, the last holding the rest. Groups are not added to the result.
    pub fn split(self, text: str, limit: int = 0) -> [str] {
        return regexSplit(self.pattern, text, limit)
    }

    /// The number of capturing groups, not counting the whole match.
    pub fn groupCount(self) -> int {
        return self.names.len() - 1
    }

    /// The names of the named groups in the order they open; groups without a name are left out.
    pub fn groupNames(self) -> [str] {
        return namedGroups(self.names)
    }

    /// True when the pattern has a group with this name, so `named(name)` does not panic.
    pub fn hasGroup(self, name: str) -> bool {
        return name.len() > 0 && self.names.contains(name)
    }
}

impl Match {
    fn slot(self, i: int) -> int {
        if i < 0 || i >= self.names.len() {
            panic("the pattern has no group {i}")
        }
        return self.base + 2 * i
    }

    /// Group `i`, 0 being the whole match, or null when it took no part in the match.
    pub fn group(self, i: int) -> str? {
        let k = self.slot(i)
        let s = self.spans[k]
        if s < 0 {
            return null
        }
        return self.subject.slice(s, self.spans[k + 1])
    }

    /// Start and end of group `i`, or null when it took no part in the match.
    pub fn span(self, i: int) -> (int, int)? {
        let k = self.slot(i)
        if self.spans[k] < 0 {
            return null
        }
        return (self.spans[k], self.spans[k + 1])
    }

    /// The group written `(?P<name>...)` or `(?<name>...)`, or null when it took no part in the match.
    pub fn named(self, name: str) -> str? {
        if let i = self.names.indexOf(name) {
            if i > 0 {
                return self.group(i)
            }
        }
        panic("the pattern has no group named '{name}'")
    }

    /// The number of capturing groups, not counting the whole match.
    pub fn groupCount(self) -> int {
        return self.names.len() - 1
    }

    /// The names of the pattern's named groups in the order they open, whether or not they took part.
    pub fn groupNames(self) -> [str] {
        return namedGroups(self.names)
    }

    /// True when the pattern has a group with this name, so `named(name)` does not panic.
    pub fn hasGroup(self, name: str) -> bool {
        return name.len() > 0 && self.names.contains(name)
    }
}
)qz";

} // namespace quartz::stdlib
