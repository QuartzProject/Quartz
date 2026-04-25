#include "Registry.hpp"

#include "HostInterface.hpp"
#include "driver/TomlLite.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <system_error>

namespace quartz::cli {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxIndex = 4u << 20;

struct CommandArgs {
    std::vector<std::string> positional;
    std::string registry;
    std::string seed;
    bool update = false;
    bool allowKeyChange = false;
    bool hostOnly = false;
};

std::optional<CommandArgs> parseCommand(int argc, char** argv) {
    CommandArgs a;
    for (int i = 2; i < argc; ++i) {
        std::string_view arg = argv[i];
        bool hasValue = i + 1 < argc;
        if (arg == "--registry" && hasValue) {
            a.registry = argv[++i];
        } else if (arg == "--sign" && hasValue) {
            a.seed = argv[++i];
        } else if (arg == "--update") {
            a.update = true;
        } else if (arg == "--allow-key-change") {
            a.allowKeyChange = true;
        } else if (arg == "--host-only") {
            a.hostOnly = true;
        } else if (arg.starts_with("--")) {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return std::nullopt;
        } else {
            a.positional.emplace_back(arg);
        }
    }
    return a;
}

int fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    return 1;
}

std::string_view textOf(std::span<const std::uint8_t> b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

// Sets `"name" = "range"` in [dependencies], keeping every other line of the file as written.
std::string withDependency(std::string_view text, const std::string& name, const std::string& range) {
    std::string entry = std::format("\"{}\" = \"{}\"", name, range);
    std::vector<std::string> lines;
    for (std::size_t start = 0; start <= text.size();) {
        std::size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            if (start < text.size()) {
                lines.emplace_back(text.substr(start));
            }
            break;
        }
        lines.emplace_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    auto trimmed = [](const std::string& l) {
        std::size_t b = l.find_first_not_of(" \t\r");
        return b == std::string::npos ? std::string() : l.substr(b, l.find_last_not_of(" \t\r") - b + 1);
    };
    std::size_t table = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (trimmed(lines[i]) == "[dependencies]") {
            table = i;
        }
    }
    if (table == lines.size()) {
        if (!lines.empty() && !trimmed(lines.back()).empty()) {
            lines.emplace_back();
        }
        lines.emplace_back("[dependencies]");
        lines.push_back(entry);
    } else {
        std::size_t insertAt = table + 1;
        bool replaced = false;
        for (std::size_t i = table + 1; i < lines.size() && !trimmed(lines[i]).starts_with('['); ++i) {
            std::string t = trimmed(lines[i]);
            if (t.starts_with("\"" + name + "\"")) {
                lines[i] = entry;
                replaced = true;
                break;
            }
            if (!t.empty()) {
                insertAt = i + 1;
            }
        }
        if (!replaced) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), entry);
        }
    }
    std::string out;
    for (const std::string& l : lines) {
        out += l + "\n";
    }
    return out;
}

std::expected<ir::SecretSeed, std::string> readSeed(const std::string& path) {
    auto text = readText(path);
    auto seed = text ? registry::fromHex<32>(*text) : std::nullopt;
    if (!seed) {
        return std::unexpected(std::format("{} does not hold a 64 digit hex seed (quartz keygen writes one)", path));
    }
    return *seed;
}

std::expected<std::vector<registry::Owner>, std::string> readOwners(const fs::path& registry) {
    auto text = readText(registry / "owners.toml");
    if (!text) {
        return std::vector<registry::Owner>{};
    }
    return registry::parseOwners(*text);
}

} // namespace

int addCommand(int argc, char** argv) {
    auto a = parseCommand(argc, argv);
    if (!a || a->positional.empty() || a->positional.size() > 2) {
        std::fprintf(stderr, "usage: quartz add <name>[@range] [package dir] [--registry url]\n");
        return 2;
    }
    const std::string& spec = a->positional[0];
    fs::path dir = a->positional.size() > 1 ? fs::path(a->positional[1]) : fs::current_path();
    std::size_t at = spec.find('@');
    std::string name = spec.substr(0, at);
    std::string range = at == std::string::npos ? std::string() : spec.substr(at + 1);
    if (!isPackageName(name)) {
        return fail(std::format("'{}' is not a package name like org.example.economy", name));
    }
    auto text = readText(dir / "package.toml");
    if (!text) {
        return fail(std::format("{} has no package.toml", dir.string()));
    }
    auto manifest = parseManifest(*text);
    if (!manifest) {
        return fail(manifest.error());
    }
    if (name == manifest->name) {
        return fail("a package cannot depend on itself");
    }
    if (range.empty()) {
        auto loc = locateRegistry(a->registry.empty() ? manifest->registry : a->registry,
                                  a->registry.empty() ? dir : fs::current_path());
        auto bytes = loc ? download(*loc, registry::indexPath(name), kMaxIndex) : std::unexpected(loc.error());
        if (!bytes) {
            return fail(bytes.error() + "; or give a range: quartz add " + name + "@^1.0");
        }
        auto releases = *bytes ? registry::parseIndex(textOf(**bytes), name)
                               : std::expected<std::vector<registry::Release>, std::string>();
        if (!releases) {
            return fail(releases.error());
        }
        auto newest = std::max_element(releases->begin(), releases->end(),
                                       [](const auto& x, const auto& y) { return x.version < y.version; });
        if (newest == releases->end()) {
            return fail(std::format("{} is not in the registry", name));
        }
        range = "^" + formatVersion(newest->version);
    }
    if (!toml::plainString(range) || !satisfies(Version{}, range)) {
        return fail(std::format("'{}' is not a version range like ^1.2 or >=1.0, <2", range));
    }
    std::string updated = withDependency(*text, name, range);
    if (auto check = parseManifest(updated); !check) {
        return fail("package.toml would become invalid: " + check.error());
    }
    std::string error;
    if (!writeAtomically(dir / "package.toml", updated, error)) {
        return fail(error);
    }
    std::printf("added %s %s; quartz fetch locks it\n", name.c_str(), range.c_str());
    return 0;
}

int fetchCommand(int argc, char** argv) {
    auto a = parseCommand(argc, argv);
    if (!a || a->positional.size() > 1) {
        std::fprintf(stderr, "usage: quartz fetch [package dir] [--registry url] [--update] [--allow-key-change]\n");
        return 2;
    }
    fs::path dir = a->positional.empty() ? fs::current_path() : fs::path(a->positional[0]);
    auto text = readText(dir / "package.toml");
    if (!text) {
        return fail(std::format("{} has no package.toml", dir.string()));
    }
    auto manifest = parseManifest(*text);
    if (!manifest) {
        return fail(manifest.error());
    }
    DependencyOptions opts{a->registry, true, a->update, a->allowKeyChange};
    auto deps = prepareDependencies(dir, *manifest, opts);
    if (!deps) {
        return fail(deps.error());
    }
    std::printf("dependencies of %s are locked and verified\n", manifest->name.c_str());
    return 0;
}

int ownerCommand(int argc, char** argv) {
    auto a = parseCommand(argc, argv);
    if (!a || a->positional.size() != 2 || a->registry.empty()) {
        std::fprintf(stderr, "usage: quartz owner <prefix, like org.example> <public key hex> --registry <dir>\n");
        return 2;
    }
    auto loc = locateRegistry(a->registry, fs::current_path());
    if (!loc || !loc->url.empty()) {
        return fail("owners are recorded in a local registry directory");
    }
    auto key = registry::fromHex<32>(a->positional[1]);
    if (!isPackageName(a->positional[0]) || !key) {
        return fail("give a prefix of two or more segments (org.example) and a 64 digit hex public key");
    }
    auto owners = readOwners(loc->local);
    if (!owners) {
        return fail(owners.error());
    }
    if (std::any_of(owners->begin(), owners->end(),
                    [&](const auto& o) { return o.prefix == a->positional[0] && o.key == *key; })) {
        std::printf("already listed\n");
        return 0;
    }
    owners->push_back({a->positional[0], *key});
    std::string error;
    if (!writeAtomically(loc->local / "owners.toml", registry::printOwners(*owners), error)) {
        return fail(error);
    }
    std::printf("key %s may publish under %s\n", a->positional[1].c_str(), a->positional[0].c_str());
    return 0;
}

int publishCommand(int argc, char** argv) {
    auto a = parseCommand(argc, argv);
    if (!a || a->positional.size() > 1 || a->registry.empty() || a->seed.empty()) {
        std::fprintf(stderr, "usage: quartz publish [package dir] --registry <dir> --sign <seed file> [--host-only]\n");
        return 2;
    }
    fs::path dir = a->positional.empty() ? fs::current_path() : fs::path(a->positional[0]);
    auto loc = locateRegistry(a->registry, fs::current_path());
    if (!loc || !loc->url.empty()) {
        return fail("publish writes to a local registry directory; upload it to the web server afterwards");
    }
    auto pkg = readPackage(dir);
    auto seed = readSeed(a->seed);
    auto owners = readOwners(loc->local);
    for (const std::string* error :
         {pkg ? nullptr : &pkg.error(), seed ? nullptr : &seed.error(), owners ? nullptr : &owners.error()}) {
        if (error != nullptr) {
            return fail(*error);
        }
    }
    const Manifest& m = pkg->manifest;
    ir::PublicKey key = ir::publicKey(*seed);
    bool owned = std::any_of(owners->begin(), owners->end(),
                             [&](const auto& o) { return o.key == key && registry::coversName(o.prefix, m.name); });
    if (!owned) {
        return fail(std::format("key {} owns no prefix of {} in {}; the registry adds it once the publisher proves "
                                "the domain (quartz owner)",
                                registry::toHex(key), m.name, (loc->local / "owners.toml").string()));
    }
    DependencyOptions opts;
    opts.registry = m.registry.empty() ? a->registry : std::string();
    auto deps = prepareDependencies(dir, m, opts);
    if (!deps) {
        return fail(deps.error());
    }
    std::vector<SourceInput> inputs;
    for (const registry::SourceFile& f : pkg->sources) {
        if (f.path != "package.toml") {
            inputs.push_back({(dir / f.path).string(), f.text});
        }
    }
    inputs.insert(inputs.end(), deps->inputs.begin(), deps->inputs.end());
    if (!a->hostOnly) {
        inputs.push_back({std::string(kHostInterfaceName), std::string(kHostInterface)});
    }
    CompileOptions co;
    co.emitBinary = m.kind == PackageKind::Plugin;
    co.manifest = pkg->manifestText;
    CompileOutput out = compile(inputs, co);
    std::fputs(out.diagnostics.c_str(), stderr);
    if (!out.program) {
        return fail(std::format("{} does not compile; nothing was published", m.name));
    }
    std::vector<std::uint8_t> archive;
    if (m.kind == PackageKind::Plugin) {
        archive = std::move(out.binary);
        if (auto s = ir::sign(archive, *seed); !s) {
            return fail(s.error());
        }
    } else {
        auto packed = registry::packSources(pkg->sources);
        if (!packed) {
            return fail(packed.error());
        }
        archive = std::move(*packed);
    }
    registry::Release r;
    r.name = m.name;
    r.version = *parseVersion(m.version);
    r.kind = m.kind;
    for (const auto& [name, range] : m.dependencies) {
        r.dependencies.push_back({name, range});
    }
    std::sort(r.dependencies.begin(), r.dependencies.end(),
              [](const auto& x, const auto& y) { return x.name < y.name; });
    r.checksum = registry::checksum(archive);
    registry::signRelease(r, *seed);
    std::vector<registry::Release> releases;
    fs::path indexFile = loc->local / registry::indexPath(m.name);
    if (auto text = readText(indexFile)) {
        auto parsed = registry::parseIndex(*text, m.name);
        if (!parsed) {
            return fail(parsed.error());
        }
        releases = std::move(*parsed);
    }
    if (std::any_of(releases.begin(), releases.end(), [&](const auto& x) { return x.version == r.version; })) {
        return fail(
            std::format("{} {} is already published; versions never change, publish a new one", m.name, m.version));
    }
    releases.push_back(r);
    std::sort(releases.begin(), releases.end(), [](const auto& x, const auto& y) { return x.version < y.version; });
    std::string error;
    if (!writeAtomically(loc->local / registry::archivePath(r), textOf(archive), error) ||
        !writeAtomically(indexFile, registry::printIndex(m.name, releases), error)) {
        return fail(error);
    }
    std::printf("published %s %s (%s, %zu bytes, blake2b %s)\n", m.name.c_str(), m.version.c_str(),
                std::string(kindName(m.kind)).c_str(), archive.size(), registry::toHex(r.checksum).c_str());
    return 0;
}

} // namespace quartz::cli
