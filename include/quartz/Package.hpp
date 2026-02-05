#pragma once

#include "vm/Bytecode.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace quartz {

/// `package.toml` (spec 8.3).
struct Manifest {
    std::string name;    // the package, e.g. `net.blueva.hub`
    std::string version; // semver `major.minor.patch`
    std::uint32_t api = 0; // lowest host API version it needs
    std::vector<std::pair<std::string, std::string>> dependencies; // package -> version range
    std::vector<std::string> capabilities;
    std::vector<std::string> exports; // functions other packages may call
};

struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    friend auto operator<=>(const Version&, const Version&) = default;
};

std::expected<Manifest, std::string> parseManifest(std::string_view text);
std::expected<Version, std::string> parseVersion(std::string_view text);
/// Ranges: `1.2.3`, `^1.2`, `~1.2`, `>=1.0`, `<2.0.0`, joined with commas (all must hold), or `*`.
std::expected<bool, std::string> satisfies(const Version& v, std::string_view range);

/// Load order for a set of packages: every dependency first; missing, mismatched or cyclic ones are errors.
std::expected<std::vector<std::size_t>, std::string> loadOrder(const std::vector<Manifest>& packages);

/// The `.qzi` other packages compile against: the exported functions of `program` under its package name.
std::expected<std::string, std::string> interfaceOf(const vm::Program& program, const Manifest& manifest);

} // namespace quartz
