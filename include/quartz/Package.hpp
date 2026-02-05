#pragma once

#include "ir/Serialize.hpp"
#include "vm/Bytecode.hpp"

#include <memory>

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

/// Which key must sign each package; packages not listed are refused unless `allowUnlisted`.
struct TrustPolicy {
    std::vector<std::pair<std::string, ir::PublicKey>> keys; // package name -> its publisher's key
    bool allowUnlisted = false;
};

struct LoadedPackage {
    Manifest manifest;
    std::shared_ptr<const vm::Program> program;
};

/// Loads a `.qzb` package: reads its manifest, then requires a valid signature by the key the policy gives
/// for that package name before anything is compiled or run.
std::expected<LoadedPackage, std::string> loadPackage(std::span<const std::uint8_t> bytes, const TrustPolicy& policy);

/// The `.qzi` other packages compile against: the exported functions of `program` under its package name.
std::expected<std::string, std::string> interfaceOf(const vm::Program& program, const Manifest& manifest);

} // namespace quartz
