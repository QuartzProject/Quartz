#include "ir/ByteIo.hpp"
#include "quartz/Registry.hpp"

#include <algorithm>
#include <format>
#include <zstd.h>

namespace quartz::registry {

namespace {

constexpr std::string_view kMagic = "QZSA";
constexpr std::uint8_t kArchiveVersion = 1;
constexpr std::size_t kMaxUnpacked = 32u << 20;
constexpr std::size_t kMaxFiles = 4096;
constexpr std::size_t kMaxPath = 255;
// Sources are small and published once; the slowest level costs milliseconds and saves bytes on every download.
constexpr int kLevel = 19;

} // namespace

bool archivablePath(std::string_view path) {
    if (path.empty() || path.size() > kMaxPath) {
        return false;
    }
    if (path != "package.toml" && !path.ends_with(".qz") && !path.ends_with(".qzi")) {
        return false;
    }
    std::string_view rest = path;
    while (true) {
        std::size_t slash = rest.find('/');
        std::string_view part = rest.substr(0, slash);
        if (part.empty() || part.front() == '.' ||
            part.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
                std::string_view::npos) {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        rest.remove_prefix(slash + 1);
    }
}

std::expected<std::vector<std::uint8_t>, std::string> packSources(std::vector<SourceFile> files) {
    std::sort(files.begin(), files.end(), [](const SourceFile& a, const SourceFile& b) { return a.path < b.path; });
    ir::bin::Writer w;
    w.u64(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!archivablePath(files[i].path) || (i > 0 && files[i - 1].path == files[i].path)) {
            return std::unexpected(std::format("'{}' cannot be packed (only package.toml, .qz and .qzi files with "
                                               "plain names)",
                                               files[i].path));
        }
        w.str(files[i].path);
        w.str(files[i].text);
    }
    if (files.size() > kMaxFiles || w.out.size() > kMaxUnpacked) {
        return std::unexpected("the package is too large to publish");
    }
    std::vector<std::uint8_t> out(kMagic.begin(), kMagic.end());
    out.push_back(kArchiveVersion);
    std::size_t header = out.size();
    out.resize(header + ZSTD_compressBound(w.out.size()));
    std::size_t n = ZSTD_compress(out.data() + header, out.size() - header, w.out.data(), w.out.size(), kLevel);
    if (ZSTD_isError(n) != 0) {
        return std::unexpected(std::format("zstd: {}", ZSTD_getErrorName(n)));
    }
    out.resize(header + n);
    return out;
}

std::expected<std::vector<SourceFile>, std::string> unpackSources(std::span<const std::uint8_t> archive) {
    std::size_t header = kMagic.size() + 1;
    if (archive.size() < header || !std::equal(kMagic.begin(), kMagic.end(), archive.begin()) ||
        archive[kMagic.size()] != kArchiveVersion) {
        return std::unexpected("not a Quartz source archive");
    }
    std::span<const std::uint8_t> frame = archive.subspan(header);
    if (ZSTD_findFrameCompressedSize(frame.data(), frame.size()) != frame.size()) {
        return std::unexpected("source archive: not exactly one zstd frame");
    }
    unsigned long long size = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR || size > kMaxUnpacked) {
        return std::unexpected("source archive: missing or excessive content size");
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(size));
    std::size_t n = ZSTD_decompress(raw.data(), raw.size(), frame.data(), frame.size());
    if (ZSTD_isError(n) != 0 || n != raw.size()) {
        return std::unexpected(
            std::format("source archive: {}", ZSTD_isError(n) != 0 ? ZSTD_getErrorName(n) : "size mismatch"));
    }
    ir::bin::Reader r(raw);
    std::uint64_t count = r.u64();
    if (count > kMaxFiles || count > r.remaining()) {
        return std::unexpected("source archive: too many files");
    }
    std::vector<SourceFile> files;
    bool manifest = false;
    for (std::uint64_t i = 0; i < count && !r.failed(); ++i) {
        SourceFile f{r.str(), r.str()};
        if (r.failed()) {
            break;
        }
        if (!archivablePath(f.path) || (!files.empty() && files.back().path >= f.path)) {
            return std::unexpected(std::format("source archive: bad or unsorted path '{}'", f.path));
        }
        manifest = manifest || f.path == "package.toml";
        files.push_back(std::move(f));
    }
    if (r.failed()) {
        return std::unexpected("source archive: " + r.error());
    }
    if (!r.atEnd() || !manifest) {
        return std::unexpected("source archive: trailing bytes or no package.toml");
    }
    return files;
}

} // namespace quartz::registry
