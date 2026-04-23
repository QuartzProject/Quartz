#pragma once

#include "quartz/Package.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Package registries (registry/FORMAT.md): the index, the lockfile, source archives and dependency resolution.
namespace quartz::registry {

struct Dependency {
    std::string name;
    std::string range;
    friend bool operator==(const Dependency&, const Dependency&) = default;
};

/// One published version as the index lists it.
struct Release {
    std::string name;
    Version version;
    PackageKind kind = PackageKind::Library;
    std::vector<Dependency> dependencies; // sorted by name
    ir::ContentHash checksum{};           // BLAKE2b-256 of the archive
    ir::PublicKey key{};                  // the publisher's Ed25519 key
    ir::Signature signature{};            // over `signedMessage`
};

/// What the publisher signs: the name, version, kind, dependencies and archive checksum.
std::string signedMessage(const Release& r);
void signRelease(Release& r, const ir::SecretSeed& seed);
[[nodiscard]] bool verifyRelease(const Release& r);

/// The index file of one package; every entry must carry `expectedName`.
std::expected<std::vector<Release>, std::string> parseIndex(std::string_view text, std::string_view expectedName);
std::string printIndex(std::string_view name, std::span<const Release> releases);

/// Paths inside a registry, built only from validated names and versions.
std::string indexPath(std::string_view name);
std::string archivePath(const Release& r);

/// `quartz.lock`: the exact packages a build uses, in load order.
struct Locked {
    std::string name;
    Version version;
    PackageKind kind = PackageKind::Library;
    std::vector<Dependency> dependencies;
    ir::ContentHash checksum{};
    ir::PublicKey key{};
};

struct Lockfile {
    std::vector<Locked> packages;
    [[nodiscard]] const Locked* find(std::string_view name) const;
};

std::expected<Lockfile, std::string> parseLock(std::string_view text);
std::string printLock(const Lockfile& lock);

/// Whether the lockfile still covers every range of the manifest and of the locked packages.
std::expected<void, std::string> lockMatches(const Manifest& root, const Lockfile& lock);

/// `owners.toml` of a registry: which key may publish under which name prefix.
struct Owner {
    std::string prefix;
    ir::PublicKey key{};
};

std::expected<std::vector<Owner>, std::string> parseOwners(std::string_view text);
std::string printOwners(std::span<const Owner> owners);
/// True when `prefix` is `name` or a dotted prefix of it.
[[nodiscard]] bool coversName(std::string_view prefix, std::string_view name);

/// A library's sources as published: `package.toml` and every `.qz`/`.qzi`, sorted, in one zstd frame.
struct SourceFile {
    std::string path; // relative, `/` separated
    std::string text;
};

std::expected<std::vector<std::uint8_t>, std::string> packSources(std::vector<SourceFile> files);
std::expected<std::vector<SourceFile>, std::string> unpackSources(std::span<const std::uint8_t> archive);
/// Whether a relative path may appear in a source archive.
[[nodiscard]] bool archivablePath(std::string_view path);

/// Every published version of a package, or an error when the registry cannot say.
using IndexLookup = std::function<std::expected<std::vector<Release>, std::string>(const std::string& name)>;

struct ResolveOptions {
    const Lockfile* previous = nullptr; // versions kept when still in range, and keys trusted on first use
    bool update = false;                // ignore the previous versions, keep only their keys
    bool allowKeyChange = false;
};

/// The highest version of each package that satisfies every range requiring it, in load order.
std::expected<Lockfile, std::string> resolve(const Manifest& root, const IndexLookup& index,
                                             const ResolveOptions& options = {});

std::string toHex(std::span<const std::uint8_t> bytes);

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> fromHex(std::string_view text);

extern template std::optional<std::array<std::uint8_t, 32>> fromHex<32>(std::string_view);
extern template std::optional<std::array<std::uint8_t, 64>> fromHex<64>(std::string_view);

ir::ContentHash checksum(std::span<const std::uint8_t> bytes);

} // namespace quartz::registry
