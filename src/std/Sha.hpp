#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace quartz::stdlib {

/// SHA-256 (FIPS 180-4), streaming.
class Sha256 {
public:
    Sha256();
    void update(std::span<const std::uint8_t> data);
    std::array<std::uint8_t, 32> final();
    void block(const std::uint8_t* p);

private:
    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buf_{};
    std::uint64_t total_ = 0;
};

/// SHA-1 (FIPS 180-4): for name-based UUIDs and old protocols, not for anything that needs collision resistance.
class Sha1 {
public:
    Sha1();
    void update(std::span<const std::uint8_t> data);
    std::array<std::uint8_t, 20> final();
    void block(const std::uint8_t* p);

private:
    std::array<std::uint32_t, 5> h_;
    std::array<std::uint8_t, 64> buf_{};
    std::uint64_t total_ = 0;
};

std::array<std::uint8_t, 32> hmacSha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);

} // namespace quartz::stdlib
