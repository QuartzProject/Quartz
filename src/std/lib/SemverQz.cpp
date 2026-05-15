#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kSemverQzi = R"qz(package org.quartzlang.semver

/// `major.minor.patch` as `package.toml` reads it: `1.2` and `1` mean `1.2.0` and `1.0.0`; no pre-release or build tags.
fn parseSemver(text: str) -> Result<Semver, str>
fn satisfiesRange(major: int, minor: int, patch: int, range: str) -> Result<bool, str>
)qz";

const std::string_view kSemverQz = R"qz(package org.quartzlang.semver

/// A version as the package manager orders and matches them.
@derive(Eq, Hash, Ord)
pub struct Semver {
    major: int
    minor: int
    patch: int
}

impl Show for Semver {
    fn show(self) -> str {
        return self.toStr()
    }
}

impl Semver {
    pub fn toStr(self) -> str {
        return "{self.major}.{self.minor}.{self.patch}"
    }

    pub fn compareTo(self, other: Semver) -> int {
        let a = [self.major, self.minor, self.patch]
        let b = [other.major, other.minor, other.patch]
        for i in 0..3 {
            if a[i] != b[i] {
                return if a[i] < b[i] { -1 } else { 1 }
            }
        }
        return 0
    }

    pub fn equals(self, other: Semver) -> bool {
        return self.compareTo(other) == 0
    }

    /// Ranges as in `package.toml`: `1.2.3` or `^1.2.3` (compatible, as Cargo), `~1.2`, `>=1.0`, `<2.0.0`, `=1.4.0`,
    /// several joined with commas (all must hold), or `*`.
    pub fn satisfies(self, range: str) -> Result<bool, str> {
        return satisfiesRange(self.major, self.minor, self.patch, range)
    }
}
)qz";

} // namespace quartz::stdlib
