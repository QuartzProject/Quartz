#include "std/lib/Library.hpp"

namespace quartz::stdlib {

const std::string_view kCryptoQzi = R"qz(package org.quartzlang.crypto

/// SHA-1, 20 bytes: for name-based UUIDs and old protocols only, it is broken for collisions.
fn sha1(data: Bytes) -> Bytes
/// SHA-256, 32 bytes.
fn sha256(data: Bytes) -> Bytes
/// SHA-512, 64 bytes.
fn sha512(data: Bytes) -> Bytes
fn hmacSha256(key: Bytes, data: Bytes) -> Bytes
fn hmacSha512(key: Bytes, data: Bytes) -> Bytes
/// BLAKE2b with a digest of `size` bytes (1 to 64).
fn blake2b(data: Bytes, size: int = 64) -> Bytes
/// Keyed BLAKE2b, a MAC; the key is at most 64 bytes.
fn blake2bKeyed(key: Bytes, data: Bytes, size: int = 64) -> Bytes
/// Equal contents, in a time that depends only on the length.
fn constantTimeEquals(a: Bytes, b: Bytes) -> bool
/// The Ed25519 public key (32 bytes) of a 32-byte secret seed.
fn publicKey(seed: Bytes) -> Result<Bytes, str>
/// A 64-byte Ed25519 signature (RFC 8032) of `message` with the key of `seed`.
fn sign(seed: Bytes, message: Bytes) -> Result<Bytes, str>
fn verify(publicKey: Bytes, message: Bytes, signature: Bytes) -> bool
/// Argon2id in the PHC string format; give it 16 random bytes of salt. The memory counts against the sandbox's.
fn hashPassword(password: str, salt: Bytes, memoryKiB: int = 19456, iterations: int = 2, parallelism: int = 1) -> Result<str, str>
/// Checks a password against a PHC string from `hashPassword` (or any Argon2 version 1.3 hash).
fn verifyPassword(password: str, hash: str) -> Result<bool, str>
)qz";

} // namespace quartz::stdlib
