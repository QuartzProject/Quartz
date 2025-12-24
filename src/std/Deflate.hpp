#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace quartz::stdlib {

/// Raw DEFLATE (RFC 1951). `inflate` never reads or writes out of bounds and stops at `maxOut` bytes.
bool inflate(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out, std::size_t maxOut, std::string& error,
             std::size_t* consumed = nullptr);
/// Level 0 stores; 1..9 use LZ77 with hash chains (longer search at higher levels) and dynamic Huffman codes.
std::vector<std::uint8_t> deflate(std::span<const std::uint8_t> in, int level);

std::uint32_t crc32(std::span<const std::uint8_t> data);
std::uint32_t adler32(std::span<const std::uint8_t> data);

} // namespace quartz::stdlib
