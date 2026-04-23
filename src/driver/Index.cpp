#include "driver/TomlLite.hpp"
#include "quartz/Registry.hpp"

#include <algorithm>
#include <format>
#include <monocypher/monocypher-ed25519.h>
#include <monocypher/monocypher.h>
#include <set>

namespace quartz::registry {

namespace {

constexpr std::size_t kMaxReleases = 10000;
constexpr std::size_t kMaxDependencies = 256;
constexpr std::int64_t kFormat = 1;

std::expected<std::string, std::string> text(const toml::Section& s, std::string_view key) {
    const toml::Entry* e = s.find(key);
    if (e == nullptr || e->kind != toml::Entry::Kind::String) {
        return std::unexpected(std::format("line {}: '{}' needs a string", s.line, key));
    }
    return e->text;
}

std::expected<void, std::string> checkFormat(const toml::Section& root, std::string_view file) {
    const toml::Entry* f = root.find("format");
    if (f == nullptr || f->kind != toml::Entry::Kind::Integer) {
        return std::unexpected(std::format("{}: missing 'format'", file));
    }
    if (f->integer != kFormat) {
        return std::unexpected(
            std::format("{}: format {} is not supported (this quartz reads {})", file, f->integer, kFormat));
    }
    return {};
}

std::expected<Version, std::string> canonicalVersion(std::string_view v) {
    auto parsed = parseVersion(v);
    if (!parsed || formatVersion(*parsed) != v) {
        return std::unexpected(std::format("'{}' is not a major.minor.patch version", v));
    }
    return *parsed;
}

std::expected<PackageKind, std::string> kindOf(std::string_view k) {
    if (k == "library") {
        return PackageKind::Library;
    }
    if (k == "plugin") {
        return PackageKind::Plugin;
    }
    return std::unexpected(std::format("kind '{}' is neither library nor plugin", k));
}

template <std::size_t N>
std::expected<std::array<std::uint8_t, N>, std::string> hexField(const toml::Section& s, std::string_view key) {
    auto t = text(s, key);
    if (!t) {
        return std::unexpected(t.error());
    }
    auto bytes = fromHex<N>(*t);
    if (!bytes) {
        return std::unexpected(std::format("line {}: '{}' needs {} hex digits", s.line, key, N * 2));
    }
    return *bytes;
}

std::expected<std::vector<Dependency>, std::string> dependencies(const toml::Section& s) {
    std::vector<Dependency> out;
    const toml::Entry* e = s.find("dependencies");
    if (e == nullptr) {
        return out;
    }
    if (e->kind != toml::Entry::Kind::Strings || e->list.size() > kMaxDependencies) {
        return std::unexpected(std::format("line {}: dependencies are a list of \"name@range\"", e->line));
    }
    for (const std::string& item : e->list) {
        std::size_t at = item.find('@');
        Dependency d{item.substr(0, at), at == std::string::npos ? std::string() : item.substr(at + 1)};
        if (at == std::string::npos || !isPackageName(d.name) || !satisfies(Version{}, d.range)) {
            return std::unexpected(std::format("line {}: bad dependency '{}'", e->line, item));
        }
        if (!out.empty() && out.back().name >= d.name) {
            return std::unexpected(std::format("line {}: dependencies must be sorted and unique", e->line));
        }
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<std::string> dependencyList(const std::vector<Dependency>& deps) {
    std::vector<std::string> out;
    for (const Dependency& d : deps) {
        out.push_back(d.name + "@" + d.range);
    }
    return out;
}

} // namespace

std::string toHex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

template <std::size_t N>
std::optional<std::array<std::uint8_t, N>> fromHex(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    if (text.size() != N * 2) {
        return std::nullopt;
    }
    auto digit = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    std::array<std::uint8_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        int hi = digit(text[2 * i]);
        int lo = digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out[i] = static_cast<std::uint8_t>(hi << 4 | lo);
    }
    return out;
}

template std::optional<std::array<std::uint8_t, 32>> fromHex<32>(std::string_view);
template std::optional<std::array<std::uint8_t, 64>> fromHex<64>(std::string_view);

ir::ContentHash checksum(std::span<const std::uint8_t> bytes) {
    ir::ContentHash h{};
    crypto_blake2b(h.data(), h.size(), bytes.data(), bytes.size());
    return h;
}

std::string signedMessage(const Release& r) {
    std::string m = std::format("quartz-registry-v1\nname {}\nversion {}\nkind {}\nchecksum {}\n", r.name,
                                formatVersion(r.version), kindName(r.kind), toHex(r.checksum));
    for (const Dependency& d : r.dependencies) {
        m += std::format("dependency {} {}\n", d.name, d.range);
    }
    return m;
}

void signRelease(Release& r, const ir::SecretSeed& seed) {
    std::array<std::uint8_t, 64> secret{};
    ir::SecretSeed scratch = seed;
    crypto_ed25519_key_pair(secret.data(), r.key.data(), scratch.data());
    std::string msg = signedMessage(r);
    crypto_ed25519_sign(r.signature.data(), secret.data(), reinterpret_cast<const std::uint8_t*>(msg.data()),
                        msg.size());
    crypto_wipe(secret.data(), secret.size());
}

bool verifyRelease(const Release& r) {
    std::string msg = signedMessage(r);
    return crypto_ed25519_check(r.signature.data(), r.key.data(), reinterpret_cast<const std::uint8_t*>(msg.data()),
                                msg.size()) == 0;
}

std::expected<std::vector<Release>, std::string> parseIndex(std::string_view source, std::string_view expectedName) {
    std::string file = "index of " + std::string(expectedName);
    auto sections = toml::parse(source, file);
    if (!sections) {
        return std::unexpected(sections.error());
    }
    if (auto f = checkFormat(sections->front(), file); !f) {
        return std::unexpected(f.error());
    }
    auto name = text(sections->front(), "name");
    if (!name || *name != expectedName) {
        return std::unexpected(std::format("{}: the file names another package", file));
    }
    std::vector<Release> out;
    std::set<Version> seen;
    for (std::size_t i = 1; i < sections->size(); ++i) {
        const toml::Section& s = (*sections)[i];
        auto fail = [&](const std::string& why) { return std::unexpected(std::format("{}: {}", file, why)); };
        if (s.name != "release" || !s.array) {
            return fail(std::format("line {}: unexpected table [{}]", s.line, s.name));
        }
        if (out.size() == kMaxReleases) {
            return fail("too many releases");
        }
        Release r;
        r.name = *name;
        auto version = text(s, "version");
        auto parsed = version ? canonicalVersion(*version) : std::unexpected(version.error());
        auto kind = text(s, "kind");
        auto parsedKind = kind ? kindOf(*kind) : std::unexpected(kind.error());
        auto deps = dependencies(s);
        auto sum = hexField<32>(s, "checksum");
        auto key = hexField<32>(s, "key");
        auto sig = hexField<64>(s, "signature");
        for (const std::string* error : {parsed ? nullptr : &parsed.error(), parsedKind ? nullptr : &parsedKind.error(),
                                         deps ? nullptr : &deps.error(), sum ? nullptr : &sum.error(),
                                         key ? nullptr : &key.error(), sig ? nullptr : &sig.error()}) {
            if (error != nullptr) {
                return fail(*error);
            }
        }
        if (!seen.insert(*parsed).second) {
            return fail(std::format("version {} is listed twice", *version));
        }
        r.version = *parsed;
        r.kind = *parsedKind;
        r.dependencies = std::move(*deps);
        r.checksum = *sum;
        r.key = *key;
        r.signature = *sig;
        out.push_back(std::move(r));
    }
    return out;
}

std::string printIndex(std::string_view name, std::span<const Release> releases) {
    std::string out = std::format("format = {}\nname = \"{}\"\n", kFormat, name);
    for (const Release& r : releases) {
        out +=
            std::format("\n[[release]]\nversion = \"{}\"\nkind = \"{}\"\n", formatVersion(r.version), kindName(r.kind));
        if (!r.dependencies.empty()) {
            out += std::format("dependencies = [{}]\n", toml::quoteList(dependencyList(r.dependencies)));
        }
        out += std::format("checksum = \"{}\"\nkey = \"{}\"\nsignature = \"{}\"\n", toHex(r.checksum), toHex(r.key),
                           toHex(r.signature));
    }
    return out;
}

std::string indexPath(std::string_view name) {
    std::string path = "index/" + std::string(name);
    std::replace(path.begin(), path.end(), '.', '/');
    return path + ".toml";
}

std::string archivePath(const Release& r) {
    std::string dir = r.name;
    std::replace(dir.begin(), dir.end(), '.', '/');
    return std::format("packages/{}/{}.{}", dir, formatVersion(r.version),
                       r.kind == PackageKind::Plugin ? "qzb" : "qzs");
}

const Locked* Lockfile::find(std::string_view name) const {
    for (const Locked& p : packages) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

std::expected<Lockfile, std::string> parseLock(std::string_view source) {
    auto sections = toml::parse(source, "quartz.lock");
    if (!sections) {
        return std::unexpected(sections.error());
    }
    if (auto f = checkFormat(sections->front(), "quartz.lock"); !f) {
        return std::unexpected(f.error());
    }
    Lockfile lock;
    std::set<std::string> names;
    for (std::size_t i = 1; i < sections->size(); ++i) {
        const toml::Section& s = (*sections)[i];
        auto fail = [&](const std::string& why) { return std::unexpected("quartz.lock: " + why); };
        if (s.name != "package" || !s.array) {
            return fail(std::format("line {}: unexpected table [{}]", s.line, s.name));
        }
        if (lock.packages.size() == kMaxReleases) {
            return fail("too many packages");
        }
        auto name = text(s, "name");
        auto version = text(s, "version");
        auto parsed = version ? canonicalVersion(*version) : std::unexpected(version.error());
        auto kind = text(s, "kind");
        auto parsedKind = kind ? kindOf(*kind) : std::unexpected(kind.error());
        auto deps = dependencies(s);
        auto sum = hexField<32>(s, "checksum");
        auto key = hexField<32>(s, "key");
        for (const std::string* error : {name ? nullptr : &name.error(), parsed ? nullptr : &parsed.error(),
                                         parsedKind ? nullptr : &parsedKind.error(), deps ? nullptr : &deps.error(),
                                         sum ? nullptr : &sum.error(), key ? nullptr : &key.error()}) {
            if (error != nullptr) {
                return fail(*error);
            }
        }
        if (!isPackageName(*name) || !names.insert(*name).second) {
            return fail(std::format("line {}: '{}' is not a package name or is listed twice", s.line, *name));
        }
        lock.packages.push_back(Locked{std::move(*name), *parsed, *parsedKind, std::move(*deps), *sum, *key});
    }
    return lock;
}

std::string printLock(const Lockfile& lock) {
    std::string out =
        std::format("# Written by quartz fetch; builds use exactly these packages.\nformat = {}\n", kFormat);
    for (const Locked& p : lock.packages) {
        out += std::format("\n[[package]]\nname = \"{}\"\nversion = \"{}\"\nkind = \"{}\"\n", p.name,
                           formatVersion(p.version), kindName(p.kind));
        if (!p.dependencies.empty()) {
            out += std::format("dependencies = [{}]\n", toml::quoteList(dependencyList(p.dependencies)));
        }
        out += std::format("checksum = \"{}\"\nkey = \"{}\"\n", toHex(p.checksum), toHex(p.key));
    }
    return out;
}

bool coversName(std::string_view prefix, std::string_view name) {
    return name == prefix || (name.size() > prefix.size() && name.starts_with(prefix) && name[prefix.size()] == '.');
}

std::expected<std::vector<Owner>, std::string> parseOwners(std::string_view source) {
    auto sections = toml::parse(source, "owners.toml");
    if (!sections) {
        return std::unexpected(sections.error());
    }
    if (auto f = checkFormat(sections->front(), "owners.toml"); !f) {
        return std::unexpected(f.error());
    }
    std::vector<Owner> out;
    for (std::size_t i = 1; i < sections->size(); ++i) {
        const toml::Section& s = (*sections)[i];
        if (s.name != "owner" || !s.array) {
            return std::unexpected(std::format("owners.toml: line {}: unexpected table [{}]", s.line, s.name));
        }
        auto prefix = text(s, "prefix");
        auto key = hexField<32>(s, "key");
        if (!prefix || !key || !isPackageName(*prefix)) {
            return std::unexpected(
                std::format("owners.toml: line {}: an owner needs a prefix like org.example and a key", s.line));
        }
        out.push_back(Owner{std::move(*prefix), *key});
    }
    return out;
}

std::string printOwners(std::span<const Owner> owners) {
    std::string out = std::format("format = {}\n", kFormat);
    for (const Owner& o : owners) {
        out += std::format("\n[[owner]]\nprefix = \"{}\"\nkey = \"{}\"\n", o.prefix, toHex(o.key));
    }
    return out;
}

} // namespace quartz::registry
