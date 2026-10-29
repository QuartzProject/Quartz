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

/// Continues `crc` (0 to start) over `data`, as zlib's crc32 does.
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);
std::uint32_t adler32(std::span<const std::uint8_t> data);

enum class Framing : std::uint8_t { Raw, Zlib, Gzip };

/// DEFLATE at `level` (clamped to 0..9) wrapped in `framing`: nothing, a zlib header and Adler-32, or a gzip member.
std::vector<std::uint8_t> compress(std::span<const std::uint8_t> in, int level, Framing framing);
/// The reverse, checking headers and checksums; false with `error` ("zlib: checksum mismatch", ...).
bool decompress(std::span<const std::uint8_t> in, Framing framing, std::vector<std::uint8_t>& out, std::size_t maxOut,
                std::string& error);

} // namespace quartz::stdlib
