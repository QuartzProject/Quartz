#include "quartz/Registry.hpp"

#include <format>
#include <map>

namespace quartz::registry {

namespace {

constexpr int kMaxRounds = 64;

struct Requirement {
    std::string range;
    std::string by; // `name version` of who asks, or the root
};

std::string describe(const std::vector<Requirement>& reqs) {
    std::string out;
    for (const Requirement& r : reqs) {
        out += std::format("{}{} (from {})", out.empty() ? "" : ", ", r.range.empty() ? "*" : r.range, r.by);
    }
    return out;
}

Manifest manifestOf(const std::string& name, const Version& v, const std::vector<Dependency>& deps) {
    Manifest m;
    m.name = name;
    m.version = formatVersion(v);
    for (const Dependency& d : deps) {
        m.dependencies.emplace_back(d.name, d.range);
    }
    return m;
}

class Resolver {
public:
    Resolver(const Manifest& root, const IndexLookup& index, const ResolveOptions& options)
        : root_(root), index_(index), options_(options) {}

    std::expected<Lockfile, std::string> run() {
        std::map<std::string, Release> chosen;
        for (int round = 0; round < kMaxRounds; ++round) {
            std::map<std::string, std::vector<Requirement>> wanted;
            std::string rootName = std::format("{} {}", root_.name, root_.version);
            for (const auto& [name, range] : root_.dependencies) {
                wanted[name].push_back({range, rootName});
            }
            for (const auto& [name, r] : chosen) {
                for (const Dependency& d : r.dependencies) {
                    wanted[d.name].push_back({d.range, std::format("{} {}", name, formatVersion(r.version))});
                }
            }
            std::map<std::string, Release> next;
            for (const auto& [name, reqs] : wanted) {
                if (name == root_.name) {
                    return std::unexpected(
                        std::format("dependency cycle: {} depends on itself through {}", name, reqs.front().by));
                }
                auto pick = choose(name, reqs);
                if (!pick) {
                    return std::unexpected(pick.error());
                }
                next.emplace(name, std::move(*pick));
            }
            if (sameVersions(next, chosen)) {
                return finish(chosen);
            }
            chosen = std::move(next);
        }
        return std::unexpected("dependency resolution did not settle; the version ranges keep moving each other");
    }

private:
    const Manifest& root_;
    const IndexLookup& index_;
    const ResolveOptions& options_;
    std::map<std::string, std::vector<Release>> releases_;

    static bool sameVersions(const std::map<std::string, Release>& a, const std::map<std::string, Release>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (auto i = a.begin(), j = b.begin(); i != a.end(); ++i, ++j) {
            if (i->first != j->first || i->second.version != j->second.version) {
                return false;
            }
        }
        return true;
    }

    std::expected<const std::vector<Release>*, std::string> lookup(const std::string& name) {
        if (auto it = releases_.find(name); it != releases_.end()) {
            return &it->second;
        }
        auto list = index_(name);
        if (!list) {
            return std::unexpected(list.error());
        }
        for (const Release& r : *list) {
            if (r.name != name || !verifyRelease(r)) {
                return std::unexpected(
                    std::format("the index entry of {} {} has an invalid signature", name, formatVersion(r.version)));
            }
        }
        return &releases_.emplace(name, std::move(*list)).first->second;
    }

    // Trust on first use: the key in the previous lockfile, else the key of the oldest published version.
    static const ir::PublicKey* trustedKey(const std::vector<Release>& list, const Locked* locked) {
        if (locked != nullptr) {
            return &locked->key;
        }
        const Release* oldest = nullptr;
        for (const Release& r : list) {
            if (oldest == nullptr || r.version < oldest->version) {
                oldest = &r;
            }
        }
        return oldest == nullptr ? nullptr : &oldest->key;
    }

    std::expected<Release, std::string> choose(const std::string& name, const std::vector<Requirement>& reqs) {
        auto list = lookup(name);
        if (!list) {
            return std::unexpected(list.error());
        }
        const Locked* locked = options_.previous != nullptr ? options_.previous->find(name) : nullptr;
        const ir::PublicKey* trusted = trustedKey(**list, locked);
        const Release* best = nullptr;
        const Release* rejectedKey = nullptr;
        for (const Release& r : **list) {
            bool ok = true;
            for (const Requirement& req : reqs) {
                auto s = satisfies(r.version, req.range);
                if (!s) {
                    return std::unexpected(std::format("{} asks for {} '{}': {}", req.by, name, req.range, s.error()));
                }
                ok = ok && *s;
            }
            if (!ok) {
                continue;
            }
            if (!options_.allowKeyChange && trusted != nullptr && r.key != *trusted) {
                rejectedKey = rejectedKey == nullptr || rejectedKey->version < r.version ? &r : rejectedKey;
                continue;
            }
            bool keep = locked != nullptr && !options_.update && r.version == locked->version;
            if (keep) {
                return r;
            }
            if (best == nullptr || best->version < r.version) {
                best = &r;
            }
        }
        if (best != nullptr && (rejectedKey == nullptr || rejectedKey->version < best->version)) {
            return *best;
        }
        if (rejectedKey != nullptr) {
            return std::unexpected(std::format(
                "{} {} is signed by key {}, not by {} which signed it before; if the publisher really changed keys, "
                "run again with --allow-key-change",
                name, formatVersion(rejectedKey->version), toHex(rejectedKey->key), toHex(*trusted)));
        }
        std::string versions;
        for (const Release& r : **list) {
            versions += (versions.empty() ? "" : ", ") + formatVersion(r.version);
        }
        if (versions.empty()) {
            return std::unexpected(std::format("{} is not in the registry (needed by {})", name, reqs.front().by));
        }
        return std::unexpected(std::format("version conflict: no version of {} satisfies {}; published: {}", name,
                                           describe(reqs), versions));
    }

    std::expected<Lockfile, std::string> finish(const std::map<std::string, Release>& chosen) {
        std::vector<Manifest> manifests;
        std::vector<const Release*> byIndex;
        for (const auto& [name, r] : chosen) {
            manifests.push_back(manifestOf(name, r.version, r.dependencies));
            byIndex.push_back(&r);
        }
        manifests.push_back(root_);
        auto order = loadOrder(manifests);
        if (!order) {
            return std::unexpected(order.error());
        }
        Lockfile lock;
        for (std::size_t i : *order) {
            if (i < byIndex.size()) {
                const Release& r = *byIndex[i];
                lock.packages.push_back(Locked{r.name, r.version, r.kind, r.dependencies, r.checksum, r.key});
            }
        }
        return lock;
    }
};

} // namespace

std::expected<Lockfile, std::string> resolve(const Manifest& root, const IndexLookup& index,
                                             const ResolveOptions& options) {
    return Resolver(root, index, options).run();
}

std::expected<void, std::string> lockMatches(const Manifest& root, const Lockfile& lock) {
    std::vector<Manifest> manifests;
    for (const Locked& p : lock.packages) {
        manifests.push_back(manifestOf(p.name, p.version, p.dependencies));
    }
    manifests.push_back(root);
    if (auto order = loadOrder(manifests); !order) {
        return std::unexpected("quartz.lock does not match package.toml (" + order.error() + "); run quartz fetch");
    }
    return {};
}

} // namespace quartz::registry
