#include "Registry.hpp"

#include <cstdlib>
#include <format>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace quartz::cli {

namespace fs = std::filesystem;

namespace {

// Every byte of a URL handed to curl is printable ASCII without quotes, spaces or backslashes.
bool safeUrl(std::string_view url) {
    for (char c : url) {
        if (c <= ' ' || c > '~' || c == '"' || c == '\'' || c == '\\' || c == '`') {
            return false;
        }
    }
    return true;
}

#ifdef _WIN32
// The C runtime splits the command line again, so each argument is quoted by its rules.
std::string quoteArgument(std::string_view arg) {
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');
    return out + "\"";
}
#endif

std::string_view curlReason(int status) {
    switch (status) {
    case 6:
        return " (host not found)";
    case 7:
        return " (connection refused)";
    case 28:
        return " (timed out)";
    case 35:
    case 60:
        return " (TLS handshake or certificate not trusted)";
    case 63:
        return " (larger than allowed)";
    default:
        return "";
    }
}

std::expected<int, std::string> runCurl(const std::vector<std::string>& args) {
#ifdef _WIN32
    std::vector<std::string> quoted;
    for (const std::string& a : args) {
        quoted.push_back(quoteArgument(a));
    }
    std::vector<const char*> argv;
    for (const std::string& a : quoted) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    intptr_t status = _spawnvp(_P_WAIT, "curl", argv.data());
    if (status < 0) {
        return std::unexpected("cannot run curl; it ships with Windows 10 and later");
    }
    return static_cast<int>(status);
#else
    std::vector<char*> argv;
    for (const std::string& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, "curl", nullptr, nullptr, argv.data(), environ) != 0) {
        return std::unexpected("cannot run curl; install it to use an https registry");
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return std::unexpected("lost track of curl");
        }
    }
    if (!WIFEXITED(status)) {
        return std::unexpected("curl was killed");
    }
    return WEXITSTATUS(status);
#endif
}

std::expected<std::optional<std::vector<std::uint8_t>>, std::string> readLimited(const fs::path& path,
                                                                                 std::size_t limit) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return std::optional<std::vector<std::uint8_t>>();
    }
    std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > limit) {
        return std::unexpected(std::format("{} is larger than {} bytes", path.string(), limit));
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        return std::unexpected("cannot read " + path.string());
    }
    return std::optional(std::move(bytes));
}

} // namespace

std::expected<RegistryLocation, std::string> locateRegistry(std::string_view spec, const fs::path& relativeTo) {
    RegistryLocation loc;
    if (spec.starts_with("https://")) {
        while (spec.ends_with('/')) {
            spec.remove_suffix(1);
        }
        if (!safeUrl(spec) || spec.size() <= 8) {
            return std::unexpected(std::format("'{}' is not a usable registry URL", spec));
        }
        loc.url = std::string(spec);
        return loc;
    }
    if (spec.find("://") != std::string_view::npos && !spec.starts_with("file://")) {
        return std::unexpected(
            std::format("registry '{}': only https://, file:// and directories are supported", spec));
    }
    if (spec.starts_with("file://")) {
        spec.remove_prefix(7);
#ifdef _WIN32
        if (spec.size() > 2 && spec[0] == '/' && spec[2] == ':') {
            spec.remove_prefix(1);
        }
#endif
    }
    if (spec.empty()) {
        return std::unexpected("no registry given: add registry = \"...\" to package.toml or pass --registry");
    }
    fs::path p(spec);
    loc.local = p.is_absolute() ? p : relativeTo / p;
    return loc;
}

std::expected<std::optional<std::vector<std::uint8_t>>, std::string>
download(const RegistryLocation& registry, const std::string& path, std::size_t limit) {
    if (registry.url.empty()) {
        return readLimited(registry.local / path, limit);
    }
    fs::path tmpDir = cacheDir() / "tmp";
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    std::random_device rd;
    fs::path out = tmpDir / std::format("download-{:08x}{:08x}", rd(), rd());
    std::string url = registry.url + "/" + path;
    std::vector<std::string> args = {"curl",       "--fail",        "--silent", "--location",     "--proto",
                                     "=https",     "--proto-redir", "=https",   "--max-filesize", std::to_string(limit),
                                     "--max-time", "300",           "--output", out.string(),     url};
    auto status = runCurl(args);
    if (!status) {
        return std::unexpected(status.error());
    }
    if (*status == 22) {
        fs::remove(out, ec);
        return std::optional<std::vector<std::uint8_t>>();
    }
    if (*status != 0) {
        fs::remove(out, ec);
        return std::unexpected(
            std::format("cannot fetch {}: curl exit status {}{}", url, *status, curlReason(*status)));
    }
    auto bytes = readLimited(out, limit);
    fs::remove(out, ec);
    if (bytes && !*bytes) {
        return std::unexpected("curl reported success but wrote nothing for " + url);
    }
    return bytes;
}

fs::path cacheDir() {
    if (const char* q = std::getenv("QUARTZ_CACHE"); q != nullptr && *q != '\0') {
        return q;
    }
#ifdef _WIN32
    if (const char* l = std::getenv("LOCALAPPDATA"); l != nullptr && *l != '\0') {
        return fs::path(l) / "quartz";
    }
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x != nullptr && *x == '/') {
        return fs::path(x) / "quartz";
    }
    if (const char* h = std::getenv("HOME"); h != nullptr && *h != '\0') {
#ifdef __APPLE__
        return fs::path(h) / "Library" / "Caches" / "quartz";
#else
        return fs::path(h) / ".cache" / "quartz";
#endif
    }
#endif
    return fs::temp_directory_path() / "quartz-cache";
}

std::optional<std::string> readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeAtomically(const fs::path& path, std::string_view bytes, std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::random_device rd;
    fs::path tmp = path;
    tmp += std::format(".tmp{:08x}", rd());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            error = "cannot write " + tmp.string();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        error = std::format("cannot write {}: {}", path.string(), ec.message());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace quartz::cli
