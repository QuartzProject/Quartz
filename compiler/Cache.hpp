#pragma once

// On-disk cache of compiled objects (D57). Integrity only: an entry is used when it is bound to the exact key and its
// bytes hash to what the entry says; the directory itself must be as trusted as the host binary.

#include <array>
#include <cstdint>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/MemoryBuffer.h>
#include <memory>
#include <string>

namespace llvm {
class TargetMachine;
}

namespace quartz::compiler {

using CacheKey = std::array<std::uint8_t, 32>;

/// Program digest, function, compiler library, LLVM version, target CPU and features, and optimization level.
CacheKey cacheKey(const std::uint8_t (&digest)[32], std::uint32_t function, const llvm::TargetMachine& tm, int opt);

/// The object stored under `key`; null when missing, truncated, corrupt or written for another key.
std::unique_ptr<llvm::MemoryBuffer> loadObject(const std::string& dir, const CacheKey& key);

/// Writes atomically (temporary file and rename); failures only cost a recompilation later.
void storeObject(const std::string& dir, const CacheKey& key, llvm::StringRef object);

void dropObject(const std::string& dir, const CacheKey& key);

} // namespace quartz::compiler
