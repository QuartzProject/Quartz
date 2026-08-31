#include "std/Builder.hpp"
#include "std/Deflate.hpp"
#include "support/Utf8.hpp"

#include <algorithm>
#include <format>
#include <zstd.h>

namespace quartz::stdlib {

namespace {

constexpr std::size_t kMaxInflate = 64u << 20;

std::span<const std::uint8_t> bytesOf(const vm::Slot& s) {
    const auto* b = static_cast<const vm::Str*>(s.p);
    return {reinterpret_cast<const std::uint8_t*>(b->data), b->len};
}

bool returnBytes(vm::HostCall& call, std::span<const std::uint8_t> data, vm::Slot* into) {
    if (!call.charge(static_cast<std::int64_t>(data.size() / 8 + 1))) {
        return false;
    }
    vm::Str* s = call.bytes(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    into->p = s;
    return s != nullptr;
}

// `Result<Bytes, str>` is a tag slot plus one payload slot.
bool okBytes(vm::HostCall& call, std::span<const std::uint8_t> data) {
    call.out[0].u = 0;
    return returnBytes(call, data, &call.out[1]);
}

bool errText(vm::HostCall& call, std::string_view message) {
    call.out[0].u = 1;
    call.out[1].p = call.string(message);
    return call.out[1].p != nullptr;
}

bool bytesLen(vm::HostCall& call, void*) {
    call.result.i = static_cast<std::int64_t>(bytesOf(call.args[0]).size());
    return true;
}

bool bytesGet(vm::HostCall& call, void*) {
    auto b = bytesOf(call.args[0]);
    std::int64_t i = call.args[1].i;
    if (i < 0 || static_cast<std::uint64_t>(i) >= b.size()) {
        call.error = std::format("index {} of {} bytes", i, b.size());
        return false;
    }
    call.result.i = b[static_cast<std::size_t>(i)];
    return true;
}

bool bytesSlice(vm::HostCall& call, void*) {
    auto b = bytesOf(call.args[0]);
    std::int64_t from = call.args[1].i;
    std::int64_t to = call.args[2].i;
    if (from < 0 || to < from || static_cast<std::uint64_t>(to) > b.size()) {
        call.error = std::format("slice [{}, {}) of {} bytes", from, to, b.size());
        return false;
    }
    return returnBytes(call, b.subspan(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)),
                       &call.result);
}

bool bytesToStr(vm::HostCall& call, void*) {
    auto b = bytesOf(call.args[0]);
    if (!validUtf8(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()))) {
        call.result.p = nullptr;
        return true;
    }
    return returnBytes(call, b, &call.result);
}

bool bytesToList(vm::HostCall& call, void*) {
    auto b = bytesOf(call.args[0]);
    vm::List* l = call.sandbox.newList(ir::kI64);
    if (l == nullptr || !call.charge(static_cast<std::int64_t>(b.size() / 8 + 1))) {
        return false;
    }
    for (std::uint8_t x : b) {
        vm::Slot v{};
        v.i = x;
        if (!call.sandbox.listPush(l, &v)) {
            return false;
        }
    }
    call.result.p = l;
    return true;
}

bool bytesHex(vm::HostCall& call, void*) {
    static constexpr char kDigits[] = "0123456789abcdef";
    auto b = bytesOf(call.args[0]);
    std::string out;
    out.reserve(b.size() * 2);
    for (std::uint8_t x : b) {
        out += kDigits[x >> 4];
        out += kDigits[x & 15];
    }
    return returnBytes(call, {reinterpret_cast<const std::uint8_t*>(out.data()), out.size()}, &call.result);
}

bool strToBytes(vm::HostCall& call, void*) { return returnBytes(call, bytesOf(call.args[0]), &call.result); }

bool bytesFromList(vm::HostCall& call, void*) {
    const auto* l = static_cast<const vm::List*>(call.args[0].p);
    std::vector<std::uint8_t> out;
    out.reserve(l->len);
    for (std::uint32_t i = 0; i < l->len; ++i) {
        std::int64_t v = l->data[i].i;
        if (v < 0 || v > 255) {
            call.error = std::format("byte value {} at {} is out of range", v, i);
            return false;
        }
        out.push_back(static_cast<std::uint8_t>(v));
    }
    return returnBytes(call, out, &call.result);
}

bool bytesConcat(vm::HostCall& call, void*) {
    const auto* l = static_cast<const vm::List*>(call.args[0].p);
    std::vector<std::uint8_t> out;
    for (std::uint32_t i = 0; i < l->len; ++i) {
        auto part = bytesOf(l->data[i]);
        out.insert(out.end(), part.begin(), part.end());
    }
    return returnBytes(call, out, &call.result);
}

bool readUInt(vm::HostCall& call, void*) {
    auto b = bytesOf(call.args[0]);
    std::int64_t at = call.args[1].i;
    std::int64_t size = call.args[2].i;
    if (size < 1 || size > 8 || at < 0 || at > static_cast<std::int64_t>(b.size()) - size) {
        call.error = std::format("cannot read {} bytes at {} of {}", size, at, b.size());
        return false;
    }
    std::uint64_t v = 0;
    for (std::int64_t k = 0; k < size; ++k) {
        std::int64_t idx = call.args[3].u != 0 ? at + k : at + size - 1 - k;
        v = (v << 8) | b[static_cast<std::size_t>(idx)];
    }
    call.result.u = v;
    return true;
}

// A copy with the low `size` bytes of the value at `at`; a write that runs past the end extends the copy.
bool writeUInt(vm::HostCall& call, void*) {
    auto b = bytesOf(call.args[0]);
    std::int64_t at = call.args[1].i;
    std::int64_t size = call.args[2].i;
    std::uint64_t v = call.args[3].u;
    if (size < 1 || size > 8 || at < 0 || at > static_cast<std::int64_t>(b.size())) {
        call.error = std::format("cannot write {} bytes at {} of {}", size, at, b.size());
        return false;
    }
    std::vector<std::uint8_t> out(b.begin(), b.end());
    out.resize(std::max(out.size(), static_cast<std::size_t>(at + size)));
    for (std::int64_t k = 0; k < size; ++k) {
        std::int64_t idx = call.args[4].u != 0 ? at + size - 1 - k : at + k;
        out[static_cast<std::size_t>(idx)] = static_cast<std::uint8_t>(v >> (8 * k));
    }
    return returnBytes(call, out, &call.result);
}

bool crc(vm::HostCall& call, void*) {
    auto b = bytesOf(call.args[0]);
    call.result.i = crc32(b);
    return call.charge(static_cast<std::int64_t>(b.size() / 16 + 1));
}

int clampLevel(std::int64_t level) { return static_cast<int>(level < 0 ? 0 : level > 9 ? 9 : level); }

bool compressRaw(vm::HostCall& call, void*) {
    auto in = bytesOf(call.args[0]);
    if (!call.charge(static_cast<std::int64_t>(in.size() / 2 + 1))) {
        return false;
    }
    return returnBytes(call, deflate(in, clampLevel(call.args[1].i)), &call.result);
}

bool decompressRaw(vm::HostCall& call, void*) {
    std::vector<std::uint8_t> out;
    std::string error;
    if (!inflate(bytesOf(call.args[0]), out, kMaxInflate, error)) {
        return errText(call, "inflate: " + error);
    }
    return okBytes(call, out);
}

bool compressZlib(vm::HostCall& call, void*) {
    auto in = bytesOf(call.args[0]);
    if (!call.charge(static_cast<std::int64_t>(in.size() / 2 + 1))) {
        return false;
    }
    std::vector<std::uint8_t> out = {0x78, 0x9C};
    std::vector<std::uint8_t> body = deflate(in, clampLevel(call.args[1].i));
    out.insert(out.end(), body.begin(), body.end());
    std::uint32_t a = adler32(in);
    for (int s = 24; s >= 0; s -= 8) {
        out.push_back(static_cast<std::uint8_t>(a >> s));
    }
    return returnBytes(call, out, &call.result);
}

bool decompressZlib(vm::HostCall& call, void*) {
    auto in = bytesOf(call.args[0]);
    if (in.size() < 6 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20) != 0) {
        return errText(call, "zlib: bad header");
    }
    std::vector<std::uint8_t> out;
    std::string error;
    std::size_t used = 0;
    if (!inflate(in.subspan(2), out, kMaxInflate, error, &used)) {
        return errText(call, "zlib: " + error);
    }
    if (2 + used + 4 > in.size()) {
        return errText(call, "zlib: missing checksum");
    }
    std::uint32_t want = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        want = (want << 8) | in[2 + used + k];
    }
    if (want != adler32(out)) {
        return errText(call, "zlib: checksum mismatch");
    }
    return okBytes(call, out);
}

bool compressGzip(vm::HostCall& call, void*) {
    auto in = bytesOf(call.args[0]);
    if (!call.charge(static_cast<std::int64_t>(in.size() / 2 + 1))) {
        return false;
    }
    std::vector<std::uint8_t> out = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
    std::vector<std::uint8_t> body = deflate(in, clampLevel(call.args[1].i));
    out.insert(out.end(), body.begin(), body.end());
    std::uint32_t c = crc32(in);
    auto size = static_cast<std::uint32_t>(in.size());
    for (int s = 0; s < 32; s += 8) {
        out.push_back(static_cast<std::uint8_t>(c >> s));
    }
    for (int s = 0; s < 32; s += 8) {
        out.push_back(static_cast<std::uint8_t>(size >> s));
    }
    return returnBytes(call, out, &call.result);
}

bool decompressGzip(vm::HostCall& call, void*) {
    auto in = bytesOf(call.args[0]);
    if (in.size() < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) {
        return errText(call, "gzip: bad header");
    }
    std::uint8_t flags = in[3];
    std::size_t pos = 10;
    if ((flags & 4) != 0) {
        if (pos + 2 > in.size()) {
            return errText(call, "gzip: truncated");
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
        return errText(call, "gzip: truncated");
    }
    std::vector<std::uint8_t> out;
    std::string error;
    std::size_t used = 0;
    if (!inflate(in.subspan(pos), out, kMaxInflate, error, &used)) {
        return errText(call, "gzip: " + error);
    }
    std::size_t tail = pos + used;
    if (tail + 8 > in.size()) {
        return errText(call, "gzip: missing trailer");
    }
    std::uint32_t want = in[tail] | (in[tail + 1] << 8) | (in[tail + 2] << 16) | (static_cast<std::uint32_t>(in[tail + 3]) << 24);
    if (want != crc32(out)) {
        return errText(call, "gzip: checksum mismatch");
    }
    return okBytes(call, out);
}

bool compressZstd(vm::HostCall& call, void*) {
    auto in = bytesOf(call.args[0]);
    if (!call.charge(static_cast<std::int64_t>(in.size() / 2 + 1))) {
        return false;
    }
    int level = static_cast<int>(std::clamp<std::int64_t>(call.args[1].i, 1, 19));
    std::vector<std::uint8_t> out(ZSTD_compressBound(in.size()));
    std::size_t n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), level);
    if (ZSTD_isError(n) != 0) {
        call.error = std::format("zstd: {}", ZSTD_getErrorName(n));
        return false;
    }
    out.resize(n);
    return returnBytes(call, out, &call.result);
}

// Streaming decode so a forged frame size cannot make it allocate past the limit.
bool decompressZstd(vm::HostCall& call, void*) {
    auto in = bytesOf(call.args[0]);
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    if (ctx == nullptr) {
        call.error = "zstd: out of memory";
        return false;
    }
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> chunk(ZSTD_DStreamOutSize());
    ZSTD_inBuffer src{in.data(), in.size(), 0};
    std::size_t last = 1;
    std::string error;
    while (src.pos < src.size || last != 0) {
        ZSTD_outBuffer dst{chunk.data(), chunk.size(), 0};
        last = ZSTD_decompressStream(ctx, &dst, &src);
        if (ZSTD_isError(last) != 0) {
            error = ZSTD_getErrorName(last);
            break;
        }
        if (out.size() + dst.pos > kMaxInflate) {
            error = "output too large";
            break;
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(dst.pos));
        if (dst.pos == 0 && src.pos == src.size && last != 0) {
            error = "truncated frame";
            break;
        }
    }
    ZSTD_freeDCtx(ctx);
    if (!error.empty()) {
        return errText(call, "zstd: " + error);
    }
    return okBytes(call, out);
}

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

bool readZip(vm::HostCall& call, void* user) {
    const auto* ctx = static_cast<const NativeCtx*>(user);
    const ir::TypeTable& types = call.sandbox.program().module->types;
    ir::TypeId listType = types.info(ctx->import->result).variants[0].payload[0];
    ir::TypeId entryType = types.info(listType).elem;
    auto in = bytesOf(call.args[0]);
    if (!call.charge(static_cast<std::int64_t>(in.size() / 8 + 1))) {
        return false;
    }
    std::size_t eocd = std::string::npos;
    for (std::size_t i = in.size() >= 22 ? in.size() - 22 : 0; in.size() >= 22; --i) {
        if (le32(in, i) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == 0 || in.size() - i > 65557) {
            break;
        }
    }
    if (eocd == std::string::npos) {
        return errText(call, "zip: no end of central directory");
    }
    std::size_t count = le16(in, eocd + 10);
    std::size_t dirAt = le32(in, eocd + 16);
    vm::List* list = call.sandbox.newList(entryType);
    if (list == nullptr) {
        return false;
    }
    std::size_t pos = dirAt;
    for (std::size_t n = 0; n < count; ++n) {
        if (pos + 46 > in.size() || le32(in, pos) != 0x02014b50) {
            return errText(call, "zip: bad central directory");
        }
        std::uint16_t method = le16(in, pos + 10);
        std::uint32_t crcWant = le32(in, pos + 16);
        std::uint32_t csize = le32(in, pos + 20);
        std::uint32_t usize = le32(in, pos + 24);
        std::size_t nameLen = le16(in, pos + 28);
        std::size_t extraLen = le16(in, pos + 30);
        std::size_t commentLen = le16(in, pos + 32);
        std::size_t local = le32(in, pos + 42);
        if (pos + 46 + nameLen > in.size() || csize == 0xFFFFFFFF || usize == 0xFFFFFFFF) {
            return errText(call, "zip: unsupported or truncated entry (zip64 is not supported)");
        }
        std::string_view name(reinterpret_cast<const char*>(in.data() + pos + 46), nameLen);
        if (!validUtf8(name)) {
            return errText(call, "zip: an entry name is not valid UTF-8");
        }
        pos += 46 + nameLen + extraLen + commentLen;
        if (local + 30 > in.size() || le32(in, local) != 0x04034b50) {
            return errText(call, "zip: bad local header");
        }
        std::size_t data = local + 30 + le16(in, local + 26) + le16(in, local + 28);
        if (data + csize > in.size() || data + csize < data) {
            return errText(call, "zip: truncated entry data");
        }
        std::vector<std::uint8_t> content;
        if (method == 0) {
            content.assign(in.begin() + static_cast<std::ptrdiff_t>(data),
                           in.begin() + static_cast<std::ptrdiff_t>(data + csize));
        } else if (method == 8) {
            std::string error;
            if (!inflate(in.subspan(data, csize), content, std::min<std::size_t>(usize, kMaxInflate), error)) {
                return errText(call, std::format("zip: {}: {}", name, error));
            }
        } else {
            return errText(call, std::format("zip: {}: compression method {} is not supported", name, method));
        }
        if (content.size() != usize || crc32(content) != crcWant) {
            return errText(call, std::format("zip: {}: checksum mismatch", name));
        }
        vm::Slot entry[2];
        entry[0].p = call.string(name);
        if (entry[0].p == nullptr || !returnBytes(call, content, &entry[1]) || !call.sandbox.listPush(list, entry)) {
            return false;
        }
    }
    call.out[0].u = 0;
    call.out[1].p = list;
    return true;
}

bool writeZip(vm::HostCall& call, void*) {
    const auto* list = static_cast<const vm::List*>(call.args[0].p);
    int level = clampLevel(call.args[1].i);
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> dir;
    auto put16 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        v.push_back(static_cast<std::uint8_t>(x));
        v.push_back(static_cast<std::uint8_t>(x >> 8));
    };
    auto put32 = [&](std::vector<std::uint8_t>& v, std::uint32_t x) {
        put16(v, x & 0xFFFF);
        put16(v, x >> 16);
    };
    for (std::uint32_t i = 0; i < list->len; ++i) {
        const vm::Slot* e = list->data + static_cast<std::size_t>(i) * list->elemSlots;
        std::string_view name = static_cast<const vm::Str*>(e[0].p)->view();
        auto content = bytesOf(e[1]);
        if (!call.charge(static_cast<std::int64_t>(content.size() / 2 + 1))) {
            return false;
        }
        std::vector<std::uint8_t> packed = level > 0 ? deflate(content, level) : std::vector<std::uint8_t>{};
        bool stored = level == 0 || packed.size() >= content.size();
        std::span<const std::uint8_t> body = stored ? content : std::span<const std::uint8_t>(packed);
        std::uint32_t c = crc32(content);
        auto offset = static_cast<std::uint32_t>(out.size());
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0x0800);
        put16(out, stored ? 0 : 8);
        put32(out, 0x00210000); // fixed timestamp (1980-01-01) keeps archives reproducible
        put32(out, c);
        put32(out, static_cast<std::uint32_t>(body.size()));
        put32(out, static_cast<std::uint32_t>(content.size()));
        put16(out, static_cast<std::uint32_t>(name.size()));
        put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), body.begin(), body.end());
        put32(dir, 0x02014b50);
        put16(dir, 20);
        put16(dir, 20);
        put16(dir, 0x0800);
        put16(dir, stored ? 0 : 8);
        put32(dir, 0x00210000);
        put32(dir, c);
        put32(dir, static_cast<std::uint32_t>(body.size()));
        put32(dir, static_cast<std::uint32_t>(content.size()));
        put16(dir, static_cast<std::uint32_t>(name.size()));
        put16(dir, 0);
        put16(dir, 0);
        put16(dir, 0);
        put16(dir, 0);
        put32(dir, 0);
        put32(dir, offset);
        dir.insert(dir.end(), name.begin(), name.end());
    }
    auto dirAt = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), dir.begin(), dir.end());
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, list->len);
    put16(out, list->len);
    put32(out, static_cast<std::uint32_t>(dir.size()));
    put32(out, dirAt);
    put16(out, 0);
    return returnBytes(call, out, &call.result);
}

} // namespace

void addBinaryNatives(std::vector<Native>& out) {
    out.insert(out.end(), {
                              {"org.quartzlang.bytes.Bytes.len", bytesLen},
                              {"org.quartzlang.bytes.Bytes.get", bytesGet},
                              {"org.quartzlang.bytes.Bytes.slice", bytesSlice},
                              {"org.quartzlang.bytes.Bytes.toStr", bytesToStr},
                              {"org.quartzlang.bytes.Bytes.toList", bytesToList},
                              {"org.quartzlang.bytes.Bytes.hex", bytesHex},
                              {"org.quartzlang.bytes.str.toBytes", strToBytes},
                              {"org.quartzlang.bytes.bytesOf", bytesFromList},
                              {"org.quartzlang.bytes.concatBytes", bytesConcat},
                              {"org.quartzlang.bytes.readUInt", readUInt},
                              {"org.quartzlang.bytes.writeUInt", writeUInt},
                              {"org.quartzlang.bytes.crc32", crc},
                              {"org.quartzlang.compress.deflate", compressRaw},
                              {"org.quartzlang.compress.inflate", decompressRaw},
                              {"org.quartzlang.compress.zlibCompress", compressZlib},
                              {"org.quartzlang.compress.zlibDecompress", decompressZlib},
                              {"org.quartzlang.compress.gzip", compressGzip},
                              {"org.quartzlang.compress.gunzip", decompressGzip},
                              {"org.quartzlang.compress.zstdCompress", compressZstd},
                              {"org.quartzlang.compress.zstdDecompress", decompressZstd},
                              {"org.quartzlang.zip.readZip", readZip},
                              {"org.quartzlang.zip.writeZip", writeZip},
                          });
}

} // namespace quartz::stdlib
