#pragma once

#include "vm/NativeAbi.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace quartz::vm {

struct Program;

/// Default reads QUARTZ_TIER (interp, auto, native); Auto and Native fall back to the interpreter without
/// libquartz_compiler.
enum class TierMode : std::uint8_t { Default, Interpreter, Auto, Native };

/// Native code and hotness counters of one program, shared by every sandbox running it.
class TierState {
public:
    explicit TierState(std::size_t functions);
    TierState(const TierState&) = delete;
    TierState& operator=(const TierState&) = delete;
    ~TierState();

    [[nodiscard]] NativeEntry entry(std::uint32_t f) const { return entries_[f].load(std::memory_order_acquire); }

    /// Adds the work of one interpreted call and queues a background compile once the function is hot.
    void charge(const std::shared_ptr<const Program>& program, std::uint32_t f, std::uint64_t fuelUsed,
                const std::string& cacheDir = {});
    /// Compiles on the calling thread, or loads the object from `cacheDir` (D70); null when it cannot be compiled.
    NativeEntry compile(const Program& program, std::uint32_t f, const std::string& cacheDir = {});
    [[nodiscard]] std::string error(std::uint32_t f) const;
    [[nodiscard]] NativeCompileStats stats(std::uint32_t f) const;

private:
    enum Status : std::uint8_t { kCold, kQueued, kReady, kFailed };

    std::size_t count_;
    std::unique_ptr<std::atomic<NativeEntry>[]> entries_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> work_;
    std::unique_ptr<std::atomic<std::uint8_t>[]> status_;
    mutable std::mutex mutex_;
    NativeJit* jit_ = nullptr;
    std::vector<std::string> errors_;
    std::vector<NativeCompileStats> stats_;
    bool digested_ = false;
    std::uint8_t digest_[32] = {};
};

/// Identifies a program for the native cache: its canonical IR and the frame facts the code generator reads.
void programDigest(const Program& program, std::uint8_t (&out)[32]);

/// Limits::nativeCacheDir, or QUARTZ_NATIVE_CACHE when that is empty.
std::string resolveCacheDir(const std::string& requested);

std::shared_ptr<TierState> makeTierState(std::size_t functions);

/// Resolves Default from the environment and drops to the interpreter when the compiler is missing.
TierMode resolveTierMode(TierMode requested);

/// Why the native tier is unavailable (library missing, or no JIT could be created); empty when it is available.
std::string nativeCompilerError();

/// Runtime callbacks handed to the compiler (NativeRuntime.cpp).
std::span<const NativeSymbol> nativeRuntimeSymbols();

} // namespace quartz::vm
