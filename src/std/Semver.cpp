#include "quartz/Package.hpp"
#include "std/Natives.hpp"

#include <charconv>
#include <format>

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

} // namespace

std::string formatVersion(const Version& v) { return std::format("{}.{}.{}", v.major, v.minor, v.patch); }

std::expected<Version, std::string> parseVersion(std::string_view text) {
    Version v;
    std::uint32_t* parts[] = {&v.major, &v.minor, &v.patch};
    text = trim(text);
    for (int i = 0; i < 3; ++i) {
        if (text.empty()) {
            if (i == 0) {
                return std::unexpected("empty version");
            }
            break;
        }
        auto r = std::from_chars(text.data(), text.data() + text.size(), *parts[i]);
        if (r.ec != std::errc{}) {
            return std::unexpected(std::format("bad version '{}'", text));
        }
        text.remove_prefix(static_cast<std::size_t>(r.ptr - text.data()));
        if (!text.empty()) {
            if (text.front() != '.' || i == 2) {
                return std::unexpected("versions are major.minor.patch");
            }
            text.remove_prefix(1);
        }
    }
    return v;
}

std::expected<bool, std::string> satisfies(const Version& v, std::string_view range) {
    range = trim(range);
    if (range == "*" || range.empty()) {
        return true;
    }
    while (!range.empty()) {
        std::size_t comma = range.find(',');
        std::string_view term = trim(range.substr(0, comma));
        range = comma == std::string_view::npos ? std::string_view{} : range.substr(comma + 1);
        std::string_view op;
        for (std::string_view candidate : {">=", "<=", ">", "<", "=", "^", "~"}) {
            if (term.starts_with(candidate)) {
                op = candidate;
                break;
            }
        }
        auto bound = parseVersion(term.substr(op.size()));
        if (!bound) {
            return std::unexpected(bound.error());
        }
        const Version& b = *bound;
        bool ok = true;
        if (op == ">=") {
            ok = v >= b;
        } else if (op == "<=") {
            ok = v <= b;
        } else if (op == ">") {
            ok = v > b;
        } else if (op == "<") {
            ok = v < b;
        } else if (op == "~") {
            ok = v >= b && v.major == b.major && v.minor == b.minor;
        } else if (op == "^" || op.empty()) {
            // Caret by default: compatible within the leftmost non-zero component, as in Cargo.
            Version upper = b.major > 0   ? Version{b.major + 1, 0, 0}
                            : b.minor > 0 ? Version{0, b.minor + 1, 0}
                                          : Version{0, 0, b.patch + 1};
            ok = v >= b && v < upper;
        } else {
            ok = v == b;
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace quartz

namespace quartz::stdlib {

namespace {

// `Result<Semver, str>`: the tag, then the three fields.
bool parseSemver(vm::HostCall& call, void*) {
    auto v = parseVersion(strArg(call.args[0]));
    if (!v) {
        return errStr(call, v.error());
    }
    call.out[0].u = 0;
    call.out[1].u = v->major;
    call.out[2].u = v->minor;
    call.out[3].u = v->patch;
    return true;
}

bool satisfiesRange(vm::HostCall& call, void*) {
    auto part = [&](int i) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(call.args[i].i, 0, UINT32_MAX));
    };
    auto ok = satisfies(Version{part(0), part(1), part(2)}, strArg(call.args[3]));
    if (!ok) {
        return errStr(call, ok.error());
    }
    return okScalar(call, *ok ? 1 : 0);
}

} // namespace

void addSemverNatives(std::vector<Native>& out) {
    out.insert(out.end(), {
                              {"org.quartzlang.semver.parseSemver", parseSemver},
                              {"org.quartzlang.semver.satisfiesRange", satisfiesRange},
                          });
}

} // namespace quartz::stdlib
