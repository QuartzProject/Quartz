#pragma once

#include "quartz/Quartz.hpp"
#include "quartz/Registry.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quartz::cli {

/// A registry as given in package.toml or on the command line: a directory, `file://` or `https://`.
struct RegistryLocation {
    std::string url;             // `https://...` without a trailing slash, empty for a local registry
    std::filesystem::path local; // the directory of a local registry
};

std::expected<RegistryLocation, std::string> locateRegistry(std::string_view spec,
                                                            const std::filesystem::path& relativeTo);

/// A file of the registry; nullopt when it does not exist. Larger than `limit` bytes is an error.
std::expected<std::optional<std::vector<std::uint8_t>>, std::string>
download(const RegistryLocation& registry, const std::string& path, std::size_t limit);

/// `$QUARTZ_CACHE`, else the platform cache directory plus `quartz`.
std::filesystem::path cacheDir();

std::optional<std::string> readText(const std::filesystem::path& path);
bool writeAtomically(const std::filesystem::path& path, std::string_view bytes, std::string& error);

/// package.toml and the sources a package publishes, paths relative to `dir`.
struct PackageFiles {
    std::string manifestText;
    Manifest manifest;
    std::vector<registry::SourceFile> sources;
};

std::expected<PackageFiles, std::string> readPackage(const std::filesystem::path& dir);

struct DependencyOptions {
    std::string registry; // overrides package.toml
    bool resolveAgain = false;
    bool update = false;
    bool allowKeyChange = false;
};

struct Plugin {
    std::string name;
    std::shared_ptr<const vm::Program> program;
};

/// What a package needs from its dependencies to compile and run.
struct Dependencies {
    std::vector<SourceInput> inputs; // library sources and plugin interfaces
    std::vector<Plugin> plugins;     // every locked plugin, in load order
};

/// Reads or writes quartz.lock, fills the cache and verifies every package against the lock.
std::expected<Dependencies, std::string> prepareDependencies(const std::filesystem::path& dir, const Manifest& manifest,
                                                             const DependencyOptions& options);

/// Sandboxes for the plugins, each linked to the plugins it imports from; `bind` adds the host functions.
class PluginHost {
public:
    std::expected<void, std::string> start(const std::vector<Plugin>& plugins,
                                           const std::function<void(vm::Sandbox&)>& bind);
    /// Links `client` to every plugin it imports from.
    std::expected<void, std::string> link(vm::Sandbox& client);

private:
    std::vector<std::string> names_;
    std::vector<std::unique_ptr<vm::Sandbox>> sandboxes_;
};

int addCommand(int argc, char** argv);
int fetchCommand(int argc, char** argv);
int publishCommand(int argc, char** argv);
int ownerCommand(int argc, char** argv);

} // namespace quartz::cli
