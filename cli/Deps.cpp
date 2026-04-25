#include "Registry.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <map>
#include <set>
#include <system_error>

namespace quartz::cli {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxIndex = 4u << 20;
constexpr std::size_t kMaxArchive = 64u << 20;

std::span<const std::uint8_t> bytesOf(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::vector<registry::Dependency> sortedDependencies(const Manifest& m) {
    std::vector<registry::Dependency> out;
    for (const auto& [name, range] : m.dependencies) {
        out.push_back({name, range});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

// The archive must describe the locked package exactly: anything else means the registry and the index disagree.
std::expected<void, std::string> sameAsLocked(const Manifest& m, const registry::Locked& p) {
    auto v = parseVersion(m.version);
    if (m.name != p.name || !v || *v != p.version || m.kind != p.kind || sortedDependencies(m) != p.dependencies) {
        return std::unexpected(
            std::format("the archive of {} {} does not match its index entry", p.name, formatVersion(p.version)));
    }
    return {};
}

std::string extension(PackageKind kind) { return kind == PackageKind::Plugin ? ".qzb" : ".qzs"; }

class Fetcher {
public:
    Fetcher(const fs::path& dir, const Manifest& manifest, const DependencyOptions& options)
        : dir_(dir), manifest_(manifest), options_(options) {}

    std::expected<RegistryLocation, std::string> registry() {
        if (!registry_) {
            bool fromCli = !options_.registry.empty();
            auto loc =
                locateRegistry(fromCli ? options_.registry : manifest_.registry, fromCli ? fs::current_path() : dir_);
            if (!loc) {
                return std::unexpected(loc.error());
            }
            registry_ = std::move(*loc);
        }
        return *registry_;
    }

    std::expected<std::vector<registry::Release>, std::string> index(const std::string& name) {
        auto reg = registry();
        if (!reg) {
            return std::unexpected(reg.error());
        }
        auto bytes = download(*reg, registry::indexPath(name), kMaxIndex);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (!*bytes) {
            return std::vector<registry::Release>{};
        }
        std::string_view text(reinterpret_cast<const char*>((*bytes)->data()), (*bytes)->size());
        return registry::parseIndex(text, name);
    }

    // The cached archive when its checksum still matches the lock, else a fresh download that must match it.
    std::expected<std::vector<std::uint8_t>, std::string> archive(const registry::Locked& p) {
        fs::path cached = cacheDir() / "archives" / (registry::toHex(p.checksum) + extension(p.kind));
        if (auto text = readText(cached); text && registry::checksum(bytesOf(*text)) == p.checksum) {
            return std::vector<std::uint8_t>(text->begin(), text->end());
        }
        auto reg = registry();
        if (!reg) {
            return std::unexpected(reg.error());
        }
        registry::Release r{p.name, p.version, p.kind, {}, {}, {}, {}};
        auto bytes = download(*reg, registry::archivePath(r), kMaxArchive);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (!*bytes) {
            return std::unexpected(
                std::format("{} {} is locked but missing from the registry", p.name, formatVersion(p.version)));
        }
        if (registry::checksum(**bytes) != p.checksum) {
            return std::unexpected(std::format("checksum mismatch for {} {}: the download is not what was published "
                                               "and locked",
                                               p.name, formatVersion(p.version)));
        }
        std::string error;
        std::string_view view(reinterpret_cast<const char*>((*bytes)->data()), (*bytes)->size());
        if (!writeAtomically(cached, view, error)) {
            std::fprintf(stderr, "warning: %s\n", error.c_str());
        }
        return std::move(**bytes);
    }

private:
    fs::path dir_;
    const Manifest& manifest_;
    const DependencyOptions& options_;
    std::optional<RegistryLocation> registry_;
};

std::expected<registry::Lockfile, std::string> lockFor(const fs::path& dir, const Manifest& manifest,
                                                       const DependencyOptions& options, Fetcher& fetcher) {
    fs::path lockPath = dir / "quartz.lock";
    std::optional<registry::Lockfile> previous;
    if (auto text = readText(lockPath)) {
        auto lock = registry::parseLock(*text);
        if (!lock) {
            return std::unexpected(lock.error());
        }
        if (!options.resolveAgain) {
            if (auto ok = registry::lockMatches(manifest, *lock); !ok) {
                return std::unexpected(ok.error());
            }
            return std::move(*lock);
        }
        previous = std::move(*lock);
    }
    registry::ResolveOptions ro;
    ro.previous = previous ? &*previous : nullptr;
    ro.update = options.update;
    ro.allowKeyChange = options.allowKeyChange;
    auto lock = registry::resolve(manifest, [&](const std::string& name) { return fetcher.index(name); }, ro);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    std::string error;
    if (!writeAtomically(lockPath, registry::printLock(*lock), error)) {
        return std::unexpected(error);
    }
    for (const registry::Locked& p : lock->packages) {
        const registry::Locked* old = previous ? previous->find(p.name) : nullptr;
        if (old == nullptr || old->version != p.version) {
            std::fprintf(stderr, "locked %s %s (%s)%s\n", p.name.c_str(), formatVersion(p.version).c_str(),
                         std::string(kindName(p.kind)).c_str(),
                         old == nullptr ? "" : std::format(", was {}", formatVersion(old->version)).c_str());
        }
    }
    return std::move(*lock);
}

// Libraries reachable from the root through libraries are compiled in; the plugins they reach are linked.
std::set<std::string> reachable(const Manifest& manifest, const registry::Lockfile& lock, bool throughPlugins) {
    std::set<std::string> out;
    std::vector<std::string> work;
    for (const auto& [name, range] : manifest.dependencies) {
        work.push_back(name);
    }
    while (!work.empty()) {
        std::string name = std::move(work.back());
        work.pop_back();
        const registry::Locked* p = lock.find(name);
        if (p == nullptr || !out.insert(name).second || (p->kind == PackageKind::Plugin && !throughPlugins)) {
            continue;
        }
        for (const registry::Dependency& d : p->dependencies) {
            work.push_back(d.name);
        }
    }
    return out;
}

} // namespace

std::expected<PackageFiles, std::string> readPackage(const fs::path& dir) {
    PackageFiles out;
    auto text = readText(dir / "package.toml");
    if (!text) {
        return std::unexpected(std::format("{} has no package.toml", dir.string()));
    }
    auto m = parseManifest(*text);
    if (!m) {
        return std::unexpected(m.error());
    }
    out.manifestText = *text;
    out.manifest = std::move(*m);
    out.sources.push_back({"package.toml", *text});
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::string leaf = it->path().filename().string();
        if (it->is_directory() && (leaf.starts_with('.') || leaf == "build")) {
            it.disable_recursion_pending();
            continue;
        }
        std::string ext = it->path().extension().string();
        if (!it->is_regular_file() || (ext != ".qz" && ext != ".qzi")) {
            continue;
        }
        std::string rel = fs::relative(it->path(), dir).generic_string();
        auto source = readText(it->path());
        if (!source) {
            return std::unexpected("cannot read " + it->path().string());
        }
        out.sources.push_back({std::move(rel), std::move(*source)});
    }
    if (ec) {
        return std::unexpected(std::format("cannot list {}: {}", dir.string(), ec.message()));
    }
    return out;
}

std::expected<Dependencies, std::string> prepareDependencies(const fs::path& dir, const Manifest& manifest,
                                                             const DependencyOptions& options) {
    Dependencies out;
    if (manifest.dependencies.empty()) {
        return out;
    }
    Fetcher fetcher(dir, manifest, options);
    auto lock = lockFor(dir, manifest, options, fetcher);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    std::set<std::string> inside = reachable(manifest, *lock, false);
    std::set<std::string> used = reachable(manifest, *lock, true);
    for (const registry::Locked& p : lock->packages) {
        if (!used.contains(p.name)) {
            continue; // left in the lock by a dependency removed since
        }
        auto bytes = fetcher.archive(p);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        std::string label = std::format("{}@{}", p.name, formatVersion(p.version));
        if (p.kind == PackageKind::Library) {
            auto files = registry::unpackSources(*bytes);
            if (!files) {
                return std::unexpected(std::format("{}: {}", label, files.error()));
            }
            auto toml =
                std::find_if(files->begin(), files->end(), [](const auto& f) { return f.path == "package.toml"; });
            auto m = parseManifest(toml->text); // unpackSources requires it
            if (!m) {
                return std::unexpected(std::format("{}: {}", label, m.error()));
            }
            if (auto same = sameAsLocked(*m, p); !same) {
                return std::unexpected(same.error());
            }
            if (!inside.contains(p.name)) {
                continue;
            }
            for (registry::SourceFile& f : *files) {
                if (f.path != "package.toml") {
                    out.inputs.push_back({label + "/" + f.path, std::move(f.text), true});
                }
            }
            continue;
        }
        TrustPolicy policy;
        policy.keys.emplace_back(p.name, p.key);
        auto loaded = loadPackage(*bytes, policy);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        if (auto same = sameAsLocked(loaded->manifest, p); !same) {
            return std::unexpected(same.error());
        }
        if (inside.contains(p.name)) {
            auto iface = interfaceOf(*loaded->program, loaded->manifest);
            if (!iface) {
                return std::unexpected(std::format("{}: {}", label, iface.error()));
            }
            out.inputs.push_back({label + ".qzi", std::move(*iface), true});
        }
        out.plugins.push_back({p.name, std::move(loaded->program)});
    }
    return out;
}

namespace {

bool importsFrom(const vm::Program& program, const std::string& package) {
    std::string prefix = package + ".";
    return std::any_of(program.module->imports.begin(), program.module->imports.end(),
                       [&](const ir::HostImport& i) { return i.name.starts_with(prefix); });
}

} // namespace

std::expected<void, std::string> PluginHost::start(const std::vector<Plugin>& plugins,
                                                   const std::function<void(vm::Sandbox&)>& bind) {
    for (const Plugin& p : plugins) {
        auto sb = std::make_unique<vm::Sandbox>(p.program);
        bind(*sb);
        if (auto l = link(*sb); !l) {
            return std::unexpected(std::format("{}: {}", p.name, l.error()));
        }
        if (auto s = sb->start(); !s) {
            return std::unexpected(std::format("{}: {}", p.name, s.error().describe()));
        }
        names_.push_back(p.name);
        sandboxes_.push_back(std::move(sb));
    }
    return {};
}

std::expected<void, std::string> PluginHost::link(vm::Sandbox& client) {
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (importsFrom(client.program(), names_[i])) {
            if (auto l = client.link(names_[i], *sandboxes_[i]); !l) {
                return std::unexpected(l.error());
            }
        }
    }
    return {};
}

} // namespace quartz::cli
