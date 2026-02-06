#pragma once

#include "ir/Ir.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quartz::ir {

/// Bumped on any change to the encoding, to the opcode, intrinsic or trap numbering, or to the builtin types.
inline constexpr std::uint16_t kFormatVersion = 4;
inline constexpr std::size_t kHeaderSize = 44;
inline constexpr std::size_t kSignatureEntrySize = 96;
inline constexpr std::uint8_t kMaxSignatures = 16;

using ContentHash = std::array<std::uint8_t, 32>;
using PublicKey = std::array<std::uint8_t, 32>;
using SecretSeed = std::array<std::uint8_t, 32>;
using Signature = std::array<std::uint8_t, 64>;

struct WriteOptions {
    bool lines = true;
};

/// Decoding limits; every count is also bounded by the bytes left in the input.
struct ReadLimits {
    std::size_t maxBytes = 8u << 20;
    std::uint32_t maxTypes = 1u << 16;
    std::uint32_t maxFunctions = 1u << 16;
    std::uint32_t maxRegisters = 0xFFFF;
    std::uint32_t maxCode = 1u << 20;
    std::uint32_t maxBlockDepth = 1024;
    std::uint32_t maxTypeDepth = 64;
};

struct ReadOptions {
    ReadLimits limits;
    std::span<const PublicKey> trustedKeys;
    /// Without it signatures are still checked when their key is trusted, but none is needed.
    bool requireSignature = false;
};

/// The module in canonical form: the same module always gives the same bytes, and each module has one encoding.
std::vector<std::uint8_t> encodePayload(const Module& m, WriteOptions options = {});
std::expected<std::unique_ptr<Module>, std::string> decodePayload(std::span<const std::uint8_t> payload,
                                                                  const ReadLimits& limits = {});

/// BLAKE2b-256 of the payload; also the key of the native code cache.
ContentHash contentHash(std::span<const std::uint8_t> payload);
/// Header, content hash, payload and an empty signature list.
std::vector<std::uint8_t> seal(std::span<const std::uint8_t> payload);
std::vector<std::uint8_t> serialize(const Module& m, WriteOptions options = {});
[[nodiscard]] bool isBinaryModule(std::span<const std::uint8_t> bytes);

PublicKey publicKey(const SecretSeed& seed);
/// Adds an Ed25519 signature over the header (which holds the content hash); earlier signatures stay.
std::expected<void, std::string> sign(std::vector<std::uint8_t>& file, const SecretSeed& seed);

/// Checks header, hash and signatures, then decodes. The result has not been validated: run ir::validate.
std::expected<std::unique_ptr<Module>, std::string> deserialize(std::span<const std::uint8_t> file,
                                                                const ReadOptions& options = {});

} // namespace quartz::ir
