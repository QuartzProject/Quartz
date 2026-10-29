#include "std/Deflate.hpp"

namespace quartz::stdlib {

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> in, int level, Framing framing) {
    level = level < 0 ? 0 : level > 9 ? 9 : level;
    if (framing == Framing::Raw) {
        return deflate(in, level);
    }
    std::vector<std::uint8_t> out = framing == Framing::Zlib
                                        ? std::vector<std::uint8_t>{0x78, 0x9C}
                                        : std::vector<std::uint8_t>{0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
    std::vector<std::uint8_t> body = deflate(in, level);
    out.insert(out.end(), body.begin(), body.end());
    if (framing == Framing::Zlib) {
        std::uint32_t a = adler32(in);
        for (int s = 24; s >= 0; s -= 8) {
            out.push_back(static_cast<std::uint8_t>(a >> s));
        }
        return out;
    }
    std::uint32_t c = crc32(in);
    auto size = static_cast<std::uint32_t>(in.size());
    for (int s = 0; s < 32; s += 8) {
        out.push_back(static_cast<std::uint8_t>(c >> s));
    }
    for (int s = 0; s < 32; s += 8) {
        out.push_back(static_cast<std::uint8_t>(size >> s));
    }
    return out;
}

namespace {

bool fail(std::string& error, std::string why) {
    error = std::move(why);
    return false;
}

bool zlib(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t maxOut, std::string& error) {
    if (in.size() < 6 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20) != 0) {
        return fail(error, "zlib: bad header");
    }
    std::size_t used = 0;
    if (!inflate(in.subspan(2), out, maxOut, error, &used)) {
        return fail(error, "zlib: " + error);
    }
    if (2 + used + 4 > in.size()) {
        return fail(error, "zlib: missing checksum");
    }
    std::uint32_t want = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        want = (want << 8) | in[2 + used + k];
    }
    return want == adler32(out) || fail(error, "zlib: checksum mismatch");
}

bool gzip(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t maxOut, std::string& error) {
    if (in.size() < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) {
        return fail(error, "gzip: bad header");
    }
    std::uint8_t flags = in[3];
    std::size_t pos = 10;
    if ((flags & 4) != 0) {
        if (pos + 2 > in.size()) {
            return fail(error, "gzip: truncated");
        }
        pos += 2 + static_cast<std::size_t>(in[pos] | (in[pos + 1] << 8));
    }
    for (std::uint8_t f : {std::uint8_t{8}, std::uint8_t{16}}) {
        if ((flags & f) != 0) {
            while (pos < in.size() && in[pos] != 0) {
                ++pos;
            }
            ++pos;
        }
    }
    if ((flags & 2) != 0) {
        pos += 2;
    }
    if (pos > in.size()) {
        return fail(error, "gzip: truncated");
    }
    std::size_t used = 0;
    if (!inflate(in.subspan(pos), out, maxOut, error, &used)) {
        return fail(error, "gzip: " + error);
    }
    std::size_t tail = pos + used;
    if (tail + 8 > in.size()) {
        return fail(error, "gzip: missing trailer");
    }
    std::uint32_t want =
        in[tail] | (in[tail + 1] << 8) | (in[tail + 2] << 16) | (static_cast<std::uint32_t>(in[tail + 3]) << 24);
    return want == crc32(out) || fail(error, "gzip: checksum mismatch");
}

} // namespace

bool decompress(std::span<const std::uint8_t> in, Framing framing, std::vector<std::uint8_t>& out, std::size_t maxOut,
                std::string& error) {
    out.clear();
    switch (framing) {
    case Framing::Zlib:
        return zlib(in, out, maxOut, error);
    case Framing::Gzip:
        return gzip(in, out, maxOut, error);
    default:
        return inflate(in, out, maxOut, error) || fail(error, "inflate: " + error);
    }
}

} // namespace quartz::stdlib
