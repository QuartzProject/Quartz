#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kTextQzi = R"qz(package org.quartzlang.text

import org.quartzlang.data.Value

/// Shell-style matching on whole strings: `*` any run, `?` one character, `[a-z]`, `[!abc]` or `[^abc]` one of a set,
/// `\` escapes. With a `separator` (`"/"` for paths), `*`, `?` and sets stay inside one segment, `**` crosses them, and
/// `**/` also matches nothing. Case-sensitive. For permission nodes keep the default: `essentials.*` then matches
/// `essentials.home` and `essentials.home.others`.
fn globMatch(pattern: str, text: str, separator: str = "") -> bool
/// Edits (insertions, deletions, substitutions of one character) that turn `a` into `b`.
fn levenshtein(a: str, b: str) -> int
/// A message in a subset of ICU MessageFormat: `{name}`, `{n, number}`, `{n, plural, =0 {none} one {# point} other {#
/// points}}` with `offset:`, and `{g, select, female {...} other {...}}`; quotes as in ICU (`''`, `'{'`). Plural rules
/// of CLDR for en, es, fr, de, ru and ar (any other language has only `other`); numbers print as Quartz prints them.
fn formatMessage(template: str, args: Map<str, Value>, locale: str = "en") -> Result<str, str>
/// The CLDR plural category (`zero`, `one`, `two`, `few`, `many` or `other`) of `n` in `locale`.
fn pluralCategory(locale: str, n: int) -> str
)qz";

const std::string_view kTextQz = R"qz(package org.quartzlang.text

/// The candidate nearest to `word` by `levenshtein`, the first one on ties, if it is within `maxDistance` edits; the
/// default allows a third of the word's length, at least 2. For "did you mean" hints.
pub fn closest(word: str, candidates: [str], maxDistance: int = -1) -> str? {
    var limit = maxDistance
    if limit < 0 {
        limit = word.chars().len() / 3
        if limit < 2 {
            limit = 2
        }
    }
    var best: str? = null
    var bestDistance = limit + 1
    for c in candidates {
        let d = levenshtein(word, c)
        if d < bestDistance {
            best = c
            bestDistance = d
        }
    }
    return best
}
)qz";

} // namespace quartz::stdlib
