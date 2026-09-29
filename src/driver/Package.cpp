#include "quartz/Package.hpp"

#include "quartz/Quartz.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <map>

namespace quartz {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// The manifest uses a small TOML subset: `key = "text" | integer | true | false | ["a", "b"]` and plain tables.
std::expected<std::string, std::string> unquote(std::string_view v) {
    v = trim(v);
    if (v.size() < 2 || v.front() != '"' || v.back() != '"' || v.substr(1, v.size() - 2).find('"') != std::string_view::npos) {
        return std::unexpected(std::format("expected a quoted string, found '{}'", v));
    }
    return std::string(v.substr(1, v.size() - 2));
}

std::expected<std::vector<std::string>, std::string> stringList(std::string_view v) {
    v = trim(v);
    if (v.size() < 2 || v.front() != '[' || v.back() != ']') {
        return std::unexpected("expected a list of strings");
    }
    std::vector<std::string> out;
    std::string_view body = v.substr(1, v.size() - 2);
    while (!trim(body).empty()) {
        std::size_t comma = body.find(',');
        auto item = unquote(body.substr(0, comma));
        if (!item) {
            return std::unexpected(item.error());
        }
        out.push_back(std::move(*item));
        if (comma == std::string_view::npos) {
            break;
        }
        body = body.substr(comma + 1);
    }
    return out;
}

} // namespace

bool isPackageName(std::string_view name) {
    std::size_t segments = 0;
    while (true) {
        std::size_t dot = name.find('.');
        std::string_view part = name.substr(0, dot);
        if (part.empty() || part.front() < 'a' || part.front() > 'z' ||
            part.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string_view::npos) {
            return false;
        }
        ++segments;
        if (dot == std::string_view::npos) {
            return segments >= 2;
        }
        name.remove_prefix(dot + 1);
    }
}

std::string_view kindName(PackageKind kind) { return kind == PackageKind::Plugin ? "plugin" : "library"; }

std::expected<Manifest, std::string> parseManifest(std::string_view text) {
    Manifest m;
    std::string table;
    std::size_t lineNo = 0;
    while (!text.empty()) {
        std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++lineNo;
        if (std::size_t hash = line.find('#'); hash != std::string_view::npos && line.substr(0, hash).find('"') == std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        auto fail = [&](const std::string& what) { return std::unexpected(std::format("package.toml:{}: {}", lineNo, what)); };
        if (line.front() == '[') {
            if (line.back() != ']') {
                return fail("unterminated table header");
            }
            table = std::string(trim(line.substr(1, line.size() - 2)));
            if (table != "dependencies" && table != "capabilities") {
                return fail(std::format("unknown table [{}]", table));
            }
            continue;
        }
        std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return fail("expected key = value");
        }
        std::string key(trim(line.substr(0, eq)));
        bool quoted = key.size() >= 2 && key.front() == '"' && key.back() == '"';
        if (quoted) {
            key = key.substr(1, key.size() - 2);
        }
        std::string_view value = trim(line.substr(eq + 1));
        if (table == "dependencies") {
            if (!quoted) {
                return fail(
                    std::format("quote the package name: \"{}\" = ... (a bare dotted key is a nested table)", key));
            }
            if (!isPackageName(key)) {
                return fail(std::format("'{}' is not a package name like org.example.economy", key));
            }
            auto range = unquote(value);
            if (!range) {
                return fail(range.error());
            }
            m.dependencies.emplace_back(key, *range);
        } else if (table == "capabilities") {
            if (value == "true") {
                m.capabilities.push_back(key);
            } else if (value != "false") {
                return fail("capabilities are true or false");
            }
        } else if (key == "name" || key == "version" || key == "registry") {
            auto s = unquote(value);
            if (!s) {
                return fail(s.error());
            }
            (key == "name" ? m.name : key == "version" ? m.version : m.registry) = *s;
        } else if (key == "kind") {
            auto s = unquote(value);
            if (!s || (*s != "library" && *s != "plugin")) {
                return fail("kind is \"library\" or \"plugin\"");
            }
            m.kind = *s == "plugin" ? PackageKind::Plugin : PackageKind::Library;
        } else if (key == "api") {
            if (std::from_chars(value.data(), value.data() + value.size(), m.api).ptr != value.data() + value.size()) {
                return fail("api is an integer");
            }
        } else if (key == "exports") {
            auto list = stringList(value);
            if (!list) {
                return fail(list.error());
            }
            m.exports = std::move(*list);
        } else {
            return fail(std::format("unknown key '{}'", key));
        }
    }
    if (m.name.empty() || m.version.empty()) {
        return std::unexpected("package.toml needs a name and a version");
    }
    if (!isPackageName(m.name)) {
        return std::unexpected(
            std::format("package.toml: '{}' is not a package name; use a reverse domain you control, "
                        "like org.example.{}",
                        m.name, m.name));
    }
    if (auto v = parseVersion(m.version); !v) {
        return std::unexpected("package.toml: " + v.error());
    }
    return m;
}

std::expected<std::vector<std::size_t>, std::string> loadOrder(const std::vector<Manifest>& packages) {
    std::map<std::string, std::size_t> byName;
    for (std::size_t i = 0; i < packages.size(); ++i) {
        if (!byName.emplace(packages[i].name, i).second) {
            return std::unexpected(std::format("package {} is loaded twice", packages[i].name));
        }
    }
    std::vector<int> state(packages.size(), 0); // 0 new, 1 visiting, 2 done
    std::vector<std::size_t> order;
    std::vector<std::string> path;
    std::string error;
    auto visit = [&](auto& self, std::size_t i) -> bool {
        if (state[i] == 2) {
            return true;
        }
        if (state[i] == 1) {
            error = "dependency cycle: ";
            for (const std::string& p : path) {
                error += p + " -> ";
            }
            error += packages[i].name;
            return false;
        }
        state[i] = 1;
        path.push_back(packages[i].name);
        for (const auto& [dep, range] : packages[i].dependencies) {
            auto it = byName.find(dep);
            if (it == byName.end()) {
                error = std::format("{} needs {} {}, which is not loaded", packages[i].name, dep, range);
                return false;
            }
            auto v = parseVersion(packages[it->second].version);
            auto ok = v ? satisfies(*v, range) : std::expected<bool, std::string>(std::unexpected(v.error()));
            if (!ok || !*ok) {
                error = ok ? std::format("{} needs {} {}, found {}", packages[i].name, dep, range,
                                         packages[it->second].version)
                           : ok.error();
                return false;
            }
            if (!self(self, it->second)) {
                return false;
            }
        }
        path.pop_back();
        state[i] = 2;
        order.push_back(i);
        return true;
    };
    for (std::size_t i = 0; i < packages.size(); ++i) {
        if (!visit(visit, i)) {
            return std::unexpected(error);
        }
    }
    return order;
}

std::expected<LoadedPackage, std::string> loadPackage(std::span<const std::uint8_t> bytes, const TrustPolicy& policy) {
    // The manifest is only read to learn which key to demand; nothing from it is trusted yet.
    auto peek = ir::deserialize(bytes, ir::ReadOptions{});
    if (!peek) {
        return std::unexpected(peek.error());
    }
    auto manifest = parseManifest((*peek)->manifest);
    if (!manifest) {
        return std::unexpected("package without a valid package.toml: " + manifest.error());
    }
    const ir::PublicKey* key = nullptr;
    for (const auto& [name, k] : policy.keys) {
        if (name == manifest->name) {
            key = &k;
        }
    }
    if (key == nullptr && !policy.allowUnlisted) {
        return std::unexpected(std::format("package {} has no trusted publisher key", manifest->name));
    }
    ir::ReadOptions ro;
    if (key != nullptr) {
        ro.trustedKeys = std::span<const ir::PublicKey>(key, 1);
        ro.requireSignature = true;
    }
    auto program = load(bytes, ro);
    if (!program) {
        return std::unexpected(std::format("package {}: {}", manifest->name, program.error()));
    }
    return LoadedPackage{std::move(*manifest), std::move(*program)};
}

} // namespace quartz
