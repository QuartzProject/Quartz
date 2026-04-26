#pragma once

// Boundary between the core and the optional native compiler (libquartz_compiler). Plain C types only: the
// compiler is loaded with dlopen and may be built against a different C++ runtime.

#include "vm/Value.hpp"

#include <cstddef>
#include <cstdint>

namespace quartz::vm {

class Sandbox;

/// Bumped whenever NativeCtx, the runtime symbols or the qc_* signatures change.
inline constexpr std::uint32_t kNativeAbi = 5;

/// Per-invocation state shared by compiled code and the runtime callbacks.
struct NativeCtx {
    Sandbox* sandbox;
    std::int64_t fuel;
    const Slot* stackEnd;            // end of the interpreter slot stack: frames are checked against it
    std::uintptr_t nativeStackLimit; // recursive native frames trap below this address
    std::uint32_t maxDepth;
    std::uint32_t trapFunction;
    std::uint32_t trapLine;
    std::uint32_t scopeLevel; // level of the innermost open `scope` region (D36)
    // Tasks (D69): normal functions called from a task overdraw down to fuelFloor before trapping.
    std::int64_t fuelFloor;
    Slot resumeValue;          // what the resumed `wait` produces
    std::uint32_t saveTop;     // slots of saved frames still to restore; nonzero only while a task resumes
    std::uint32_t suspending;  // a failed return is a suspension, not a trap
    std::uint32_t resumeState; // state of the frame record restored last
    std::uint32_t pad2;
};

inline constexpr std::uint32_t kNoTrapFunction = 0xFFFFFFFF;
/// Code passed to the trap callback when a runtime callback already recorded the trap.
inline constexpr std::uint32_t kTrapAlreadySet = 0xFF;

/// Compiled entry: arguments in frame[0..paramSlots), result written to frame[0..resultSlots); 0 on success.
using NativeEntry = std::int32_t (*)(Slot* frame, NativeCtx* ctx);

/// A runtime function the compiled code links against by name.
struct NativeSymbol {
    const char* name;
    void* address;
};

struct NativeCompileStats {
    double totalMs;
    double lowerMs;
    double optimizeMs;
    std::uint32_t functions;
    std::uint32_t irInstructions;
    std::uint32_t cache; // NativeCache* below
    std::uint32_t pad;
};

inline constexpr std::uint32_t kNativeCacheOff = 0;
inline constexpr std::uint32_t kNativeCacheHit = 1;
inline constexpr std::uint32_t kNativeCacheStored = 2;

/// On-disk object cache (D70): `digest` identifies the program and everything the code generator reads from it.
struct NativeCacheRequest {
    const char* dir;
    std::uint8_t digest[32];
};

struct NativeJit;

// The compiler library exports these four functions with C linkage.
using QcAbiFn = std::uint32_t (*)();
/// optLevel 0 to 3; returns null and fills `err` on failure.
using QcCreateFn = NativeJit* (*)(const NativeSymbol* runtime, std::uint32_t count, int optLevel, char* err,
                                  std::size_t errLen);
/// `program` is a `const vm::Program*` built by the same Quartz version (checked through kNativeAbi); `cache` may be
/// null. A JIT serves one program only.
using QcCompileFn = int (*)(NativeJit* jit, const void* program, std::uint32_t function,
                            const NativeCacheRequest* cache, NativeEntry* entry, NativeCompileStats* stats, char* err,
                            std::size_t errLen);
using QcDestroyFn = void (*)(NativeJit* jit);

} // namespace quartz::vm
