#include "ir/Serialize.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <monocypher/monocypher-ed25519.h>
#include <monocypher/monocypher.h>
#include <string_view>

namespace quartz::ir {

namespace {

constexpr std::uint8_t kMagic[4] = {'Q', 'Z', 'I', 'R'};
constexpr std::string_view kSignatureDomain = "quartz-ir-signature-v1";
constexpr std::size_t kHashOffset = 12;

std::uint32_t readLe(std::span<const std::uint8_t> b, std::size_t at, int bytes) {
    std::uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v |= static_cast<std::uint32_t>(b[at + i]) << (8 * i);
    }
    return v;
}

std::vector<std::uint8_t> signedMessage(std::span<const std::uint8_t> header) {
    std::vector<std::uint8_t> msg(kSignatureDomain.begin(), kSignatureDomain.end());
    msg.insert(msg.end(), header.begin(), header.begin() + kHeaderSize);
    return msg;
}

struct Parsed {
    std::span<const std::uint8_t> payload;
    std::span<const std::uint8_t> signatures;
    std::uint8_t signatureCount = 0;
};

std::expected<Parsed, std::string> parse(std::span<const std::uint8_t> file, std::size_t maxBytes) {
    if (!isBinaryModule(file)) {
        return std::unexpected("not a Quartz IR module");
    }
    if (file.size() < kHeaderSize + 1) {
        return std::unexpected("truncated header");
    }
    if (readLe(file, 4, 2) != kFormatVersion) {
        return std::unexpected(
            std::format("unsupported format version {} (this host reads {})", readLe(file, 4, 2), kFormatVersion));
    }
    if (readLe(file, 6, 2) != 0) {
        return std::unexpected("unknown header flags");
    }
    std::size_t len = readLe(file, 8, 4);
    if (len > maxBytes) {
        return std::unexpected("module too large");
    }
    if (len > file.size() - kHeaderSize - 1) {
        return std::unexpected("truncated payload");
    }
    Parsed p;
    p.payload = file.subspan(kHeaderSize, len);
    p.signatureCount = file[kHeaderSize + len];
    p.signatures = file.subspan(kHeaderSize + len + 1);
    if (p.signatureCount > kMaxSignatures || p.signatures.size() != p.signatureCount * kSignatureEntrySize) {
        return std::unexpected("malformed signature list");
    }
    ContentHash h = contentHash(p.payload);
    if (std::memcmp(h.data(), file.data() + kHashOffset, h.size()) != 0) {
        return std::unexpected("content hash mismatch: the module is corrupt");
    }
    return p;
}

} // namespace

ContentHash contentHash(std::span<const std::uint8_t> payload) {
    ContentHash h{};
    crypto_blake2b(h.data(), h.size(), payload.data(), payload.size());
    return h;
}

bool isBinaryModule(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= sizeof(kMagic) && std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0;
}

std::vector<std::uint8_t> seal(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> out(kMagic, kMagic + sizeof(kMagic));
    auto le = [&](std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    le(kFormatVersion, 2);
    le(0, 2);
    le(static_cast<std::uint32_t>(payload.size()), 4);
    ContentHash h = contentHash(payload);
    out.insert(out.end(), h.begin(), h.end());
    out.insert(out.end(), payload.begin(), payload.end());
    out.push_back(0);
    return out;
}

std::vector<std::uint8_t> serialize(const Module& m, WriteOptions options) { return seal(encodePayload(m, options)); }

PublicKey publicKey(const SecretSeed& seed) {
    std::uint8_t secret[64];
    PublicKey key{};
    SecretSeed scratch = seed;
    crypto_ed25519_key_pair(secret, key.data(), scratch.data());
    crypto_wipe(secret, sizeof(secret));
    return key;
}

std::expected<void, std::string> sign(std::vector<std::uint8_t>& file, const SecretSeed& seed) {
    auto parsed = parse(file, SIZE_MAX);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (parsed->signatureCount == kMaxSignatures) {
        return std::unexpected("too many signatures");
    }
    std::size_t countAt = kHeaderSize + parsed->payload.size();
    std::uint8_t secret[64];
    PublicKey key{};
    SecretSeed scratch = seed;
    crypto_ed25519_key_pair(secret, key.data(), scratch.data());
    std::vector<std::uint8_t> msg = signedMessage(file);
    Signature sig{};
    crypto_ed25519_sign(sig.data(), secret, msg.data(), msg.size());
    crypto_wipe(secret, sizeof(secret));
    ++file[countAt];
    file.insert(file.end(), key.begin(), key.end());
    file.insert(file.end(), sig.begin(), sig.end());
    return {};
}

std::expected<std::unique_ptr<Module>, std::string> deserialize(std::span<const std::uint8_t> file,
                                                                const ReadOptions& options) {
    auto parsed = parse(file, options.limits.maxBytes);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    std::vector<std::uint8_t> msg = signedMessage(file);
    bool trusted = false;
    for (std::size_t i = 0; i < parsed->signatureCount; ++i) {
        std::span<const std::uint8_t> entry = parsed->signatures.subspan(i * kSignatureEntrySize, kSignatureEntrySize);
        bool known = std::ranges::any_of(
            options.trustedKeys, [&](const PublicKey& k) { return std::equal(k.begin(), k.end(), entry.begin()); });
        if (!known) {
            continue;
        }
        if (crypto_ed25519_check(entry.data() + 32, entry.data(), msg.data(), msg.size()) != 0) {
            return std::unexpected("invalid signature from a trusted key");
        }
        trusted = true;
    }
    if (options.requireSignature && !trusted) {
        return std::unexpected("the module is not signed by a trusted key");
    }
    return decodePayload(parsed->payload, options.limits);
}

} // namespace quartz::ir
