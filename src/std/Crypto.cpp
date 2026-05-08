#include "std/Natives.hpp"
#include "std/Sha.hpp"

#include <charconv>
#include <format>
#include <monocypher/monocypher-ed25519.h>
#include <monocypher/monocypher.h>
#include <vector>

namespace quartz::stdlib {

namespace {

// Fuel per byte or per operation, measured so that one unit costs about what a backward jump does (D109).
constexpr std::size_t kSha256BytesPerUnit = 1;
constexpr std::size_t kShaBytesPerUnit = 2;
constexpr std::size_t kBlakeBytesPerUnit = 4;
constexpr std::int64_t kSignUnits = 8'000;
constexpr std::int64_t kVerifyUnits = 22'000;
constexpr std::int64_t kArgonUnitsPerBlock = 200;

constexpr std::int64_t kArgonMaxLanes = 16;
constexpr std::int64_t kArgonMaxPasses = 1'000;
constexpr std::size_t kArgonHashBytes = 32;

bool chargeRead(vm::HostCall& call, std::size_t bytes, std::size_t bytesPerUnit) {
    return call.charge(static_cast<std::int64_t>(bytes / bytesPerUnit + 1));
}

bool sha1(vm::HostCall& call, void*) {
    auto in = bytesArg(call.args[0]);
    if (!chargeRead(call, in.size(), kShaBytesPerUnit)) {
        return false;
    }
    Sha1 h;
    h.update(in);
    return putBytes(call, h.final(), &call.result);
}

bool sha256(vm::HostCall& call, void*) {
    auto in = bytesArg(call.args[0]);
    if (!chargeRead(call, in.size(), kSha256BytesPerUnit)) {
        return false;
    }
    Sha256 h;
    h.update(in);
    return putBytes(call, h.final(), &call.result);
}

bool sha512(vm::HostCall& call, void*) {
    auto in = bytesArg(call.args[0]);
    if (!chargeRead(call, in.size(), kShaBytesPerUnit)) {
        return false;
    }
    std::uint8_t out[64];
    crypto_sha512(out, in.data(), in.size());
    return putBytes(call, out, &call.result);
}

bool hmac256(vm::HostCall& call, void*) {
    auto key = bytesArg(call.args[0]);
    auto in = bytesArg(call.args[1]);
    if (!chargeRead(call, key.size() + in.size() + 256, kSha256BytesPerUnit)) {
        return false;
    }
    return putBytes(call, hmacSha256(key, in), &call.result);
}

bool hmac512(vm::HostCall& call, void*) {
    auto key = bytesArg(call.args[0]);
    auto in = bytesArg(call.args[1]);
    if (!chargeRead(call, key.size() + in.size() + 512, kShaBytesPerUnit)) {
        return false;
    }
    std::uint8_t out[64];
    crypto_sha512_hmac(out, key.data(), key.size(), in.data(), in.size());
    return putBytes(call, out, &call.result);
}

bool blake2bWith(vm::HostCall& call, std::span<const std::uint8_t> key, std::span<const std::uint8_t> in,
                 std::int64_t size) {
    if (size < 1 || size > 64) {
        return misuse(call, std::format("blake2b size must be 1 to 64 bytes, got {}", size));
    }
    if (key.size() > 64) {
        return misuse(call, std::format("blake2b keys are at most 64 bytes, got {}", key.size()));
    }
    if (!chargeRead(call, in.size(), kBlakeBytesPerUnit)) {
        return false;
    }
    std::uint8_t out[64];
    crypto_blake2b_keyed(out, static_cast<std::size_t>(size), key.data(), key.size(), in.data(), in.size());
    return putBytes(call, std::span<const std::uint8_t>(out, static_cast<std::size_t>(size)), &call.result);
}

bool blake2b(vm::HostCall& call, void*) { return blake2bWith(call, {}, bytesArg(call.args[0]), call.args[1].i); }

bool blake2bKeyed(vm::HostCall& call, void*) {
    return blake2bWith(call, bytesArg(call.args[0]), bytesArg(call.args[1]), call.args[2].i);
}

bool constantTimeEquals(vm::HostCall& call, void*) {
    auto a = bytesArg(call.args[0]);
    auto b = bytesArg(call.args[1]);
    if (!call.charge(static_cast<std::int64_t>(a.size() / 16 + 1))) {
        return false;
    }
    if (a.size() != b.size()) {
        call.result.u = 0;
        return true;
    }
    volatile std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<std::uint8_t>(diff | (a[i] ^ b[i]));
    }
    call.result.u = diff == 0 ? 1 : 0;
    return true;
}

// Monocypher's key pair call wipes the seed it gets, so it gets a copy; the secret key is wiped after use.
struct KeyPair {
    std::uint8_t secret[64];
    std::uint8_t pub[32];

    explicit KeyPair(std::span<const std::uint8_t> seed) {
        std::uint8_t copy[32];
        std::copy(seed.begin(), seed.end(), copy);
        crypto_ed25519_key_pair(secret, pub, copy);
    }

    ~KeyPair() { crypto_wipe(secret, sizeof secret); }

    KeyPair(const KeyPair&) = delete;
    KeyPair& operator=(const KeyPair&) = delete;
};

bool publicKey(vm::HostCall& call, void*) {
    auto seed = bytesArg(call.args[0]);
    if (seed.size() != 32) {
        return errStr(call, std::format("an Ed25519 seed is 32 bytes, got {}", seed.size()));
    }
    if (!call.charge(kSignUnits)) {
        return false;
    }
    KeyPair keys(seed);
    return okBytes(call, keys.pub);
}

bool sign(vm::HostCall& call, void*) {
    auto seed = bytesArg(call.args[0]);
    auto message = bytesArg(call.args[1]);
    if (seed.size() != 32) {
        return errStr(call, std::format("an Ed25519 seed is 32 bytes, got {}", seed.size()));
    }
    if (!call.charge(2 * kSignUnits) || !chargeRead(call, 2 * message.size(), kShaBytesPerUnit)) {
        return false;
    }
    KeyPair keys(seed);
    std::uint8_t signature[64];
    crypto_ed25519_sign(signature, keys.secret, message.data(), message.size());
    return okBytes(call, signature);
}

bool verify(vm::HostCall& call, void*) {
    auto key = bytesArg(call.args[0]);
    auto message = bytesArg(call.args[1]);
    auto signature = bytesArg(call.args[2]);
    if (!call.charge(kVerifyUnits) || !chargeRead(call, message.size(), kShaBytesPerUnit)) {
        return false;
    }
    call.result.u = key.size() == 32 && signature.size() == 64 &&
                    crypto_ed25519_check(signature.data(), key.data(), message.data(), message.size()) == 0;
    return true;
}

struct ArgonParams {
    std::uint32_t algorithm = CRYPTO_ARGON2_ID;
    std::int64_t memoryKiB = 0;
    std::int64_t passes = 0;
    std::int64_t lanes = 0;
};

constexpr std::string_view kArgonNames[] = {"argon2d", "argon2i", "argon2id"};

// Checks the parameters against the limits and the sandbox's free memory, charges the fuel and computes the hash.
bool argon2(vm::HostCall& call, const ArgonParams& p, std::string_view password, std::span<const std::uint8_t> salt,
            std::span<std::uint8_t> hash, std::string& error) {
    if (p.lanes < 1 || p.lanes > kArgonMaxLanes) {
        error = std::format("parallelism must be 1 to {}, got {}", kArgonMaxLanes, p.lanes);
        return false;
    }
    if (p.passes < 1 || p.passes > kArgonMaxPasses) {
        error = std::format("iterations must be 1 to {}, got {}", kArgonMaxPasses, p.passes);
        return false;
    }
    if (p.memoryKiB < 8 * p.lanes || p.memoryKiB > (std::int64_t{1} << 32) - 1) {
        error = std::format("memory must be at least 8 KiB per lane, got {} KiB", p.memoryKiB);
        return false;
    }
    if (salt.size() < 8 || salt.size() > 1024) {
        error = std::format("the salt must be 8 to 1024 bytes (16 random bytes are usual), got {}", salt.size());
        return false;
    }
    auto bytes = static_cast<std::size_t>(p.memoryKiB) * 1024;
    std::size_t free = call.sandbox.memoryLimit() - call.sandbox.memoryUsed();
    if (bytes > free) {
        error = std::format("argon2 needs {} KiB but the sandbox has {} KiB free", p.memoryKiB, free / 1024);
        return false;
    }
    if (!call.charge(p.memoryKiB * p.passes * kArgonUnitsPerBlock)) {
        return false;
    }
    std::vector<std::uint64_t> work(bytes / 8);
    crypto_argon2_config config{p.algorithm, static_cast<std::uint32_t>(p.memoryKiB),
                                static_cast<std::uint32_t>(p.passes), static_cast<std::uint32_t>(p.lanes)};
    crypto_argon2_inputs inputs{reinterpret_cast<const std::uint8_t*>(password.data()), salt.data(),
                                static_cast<std::uint32_t>(password.size()), static_cast<std::uint32_t>(salt.size())};
    crypto_argon2(hash.data(), static_cast<std::uint32_t>(hash.size()), work.data(), config, inputs,
                  crypto_argon2_no_extras);
    crypto_wipe(work.data(), bytes);
    return true;
}

bool hashPassword(vm::HostCall& call, void*) {
    ArgonParams p{CRYPTO_ARGON2_ID, call.args[2].i, call.args[3].i, call.args[4].i};
    auto salt = bytesArg(call.args[1]);
    std::uint8_t hash[kArgonHashBytes];
    std::string error;
    if (!argon2(call, p, strArg(call.args[0]), salt, hash, error)) {
        return error.empty() ? false : errStr(call, error);
    }
    std::string text = std::format("$argon2id$v=19$m={},t={},p={}${}${}", p.memoryKiB, p.passes, p.lanes,
                                   base64Encode(salt, false, false), base64Encode(hash, false, false));
    return okStr(call, text);
}

std::optional<std::int64_t> number(std::string_view& s, std::string_view key) {
    if (!s.starts_with(key)) {
        return std::nullopt;
    }
    s.remove_prefix(key.size());
    std::int64_t v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || s.front() == '+' || s.front() == '-') {
        return std::nullopt;
    }
    s.remove_prefix(static_cast<std::size_t>(r.ptr - s.data()));
    return v;
}

// The PHC string format of the reference implementation: $argon2id$v=19$m=65536,t=3,p=4$salt$hash.
bool verifyPassword(vm::HostCall& call, void*) {
    std::string_view text = strArg(call.args[1]);
    std::vector<std::string_view> parts;
    for (std::size_t at = 0; at <= text.size();) {
        std::size_t next = text.find('$', at);
        parts.push_back(text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        at = next == std::string_view::npos ? text.size() + 1 : next + 1;
    }
    if (parts.size() == 5) {
        parts.insert(parts.begin() + 2, "v=19");
    }
    if (parts.size() != 6 || !parts[0].empty()) {
        return errStr(call, "not an argon2 hash: expected $argon2id$v=19$m=..,t=..,p=..$salt$hash");
    }
    ArgonParams p;
    std::size_t alg = 0;
    while (alg < 3 && parts[1] != kArgonNames[alg]) {
        ++alg;
    }
    if (alg == 3) {
        return errStr(call, std::format("unknown algorithm '{}'", parts[1]));
    }
    p.algorithm = static_cast<std::uint32_t>(alg);
    if (parts[2] != "v=19") {
        return errStr(call, "only argon2 version 19 (1.3) is supported");
    }
    std::string_view params = parts[3];
    auto m = number(params, "m=");
    auto t = params.starts_with(',') ? (params.remove_prefix(1), number(params, "t=")) : std::nullopt;
    auto l = params.starts_with(',') ? (params.remove_prefix(1), number(params, "p=")) : std::nullopt;
    if (!m || !t || !l || !params.empty()) {
        return errStr(call, std::format("bad argon2 parameters '{}'", parts[3]));
    }
    p.memoryKiB = *m;
    p.passes = *t;
    p.lanes = *l;
    std::vector<std::uint8_t> salt;
    std::vector<std::uint8_t> want;
    std::string error;
    if (!base64Decode(parts[4], false, salt, error) || !base64Decode(parts[5], false, want, error)) {
        return errStr(call, "bad base64 in the hash: " + error);
    }
    if (want.size() < 4 || want.size() > 64) {
        return errStr(call, std::format("hash length must be 4 to 64 bytes, got {}", want.size()));
    }
    std::vector<std::uint8_t> got(want.size());
    if (!argon2(call, p, strArg(call.args[0]), salt, got, error)) {
        return error.empty() ? false : errStr(call, error);
    }
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < got.size(); ++i) {
        diff = static_cast<std::uint8_t>(diff | (got[i] ^ want[i]));
    }
    return okScalar(call, diff == 0 ? 1 : 0);
}

} // namespace

void addCryptoNatives(std::vector<Native>& out) {
    out.insert(out.end(), {
                              {"org.quartzlang.crypto.sha1", sha1},
                              {"org.quartzlang.crypto.sha256", sha256},
                              {"org.quartzlang.crypto.sha512", sha512},
                              {"org.quartzlang.crypto.hmacSha256", hmac256},
                              {"org.quartzlang.crypto.hmacSha512", hmac512},
                              {"org.quartzlang.crypto.blake2b", blake2b},
                              {"org.quartzlang.crypto.blake2bKeyed", blake2bKeyed},
                              {"org.quartzlang.crypto.constantTimeEquals", constantTimeEquals},
                              {"org.quartzlang.crypto.publicKey", publicKey},
                              {"org.quartzlang.crypto.sign", sign},
                              {"org.quartzlang.crypto.verify", verify},
                              {"org.quartzlang.crypto.hashPassword", hashPassword},
                              {"org.quartzlang.crypto.verifyPassword", verifyPassword},
                          });
}

} // namespace quartz::stdlib
