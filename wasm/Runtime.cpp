#include "Run.hpp"
#include "quartz/Quartz.hpp"

#include <emscripten/emscripten.h>
#include <span>

extern "C" {

/// Loads a `.qzb` module (no front end in this build) and runs `entry` like `qzp_run`.
EMSCRIPTEN_KEEPALIVE int qzr_run(const std::uint8_t* bytes, int length, const char* entry, double fuelSlice,
                                 double maxSlices, double outputLimit) {
    auto program = quartz::load(std::span(bytes, static_cast<std::size_t>(length)));
    if (!program) {
        quartz::wasm::emit(quartz::wasm::kTrap, program.error());
        return quartz::wasm::kInvalidModule;
    }
    return quartz::wasm::run(*program, entry, fuelSlice, maxSlices, outputLimit);
}

} // extern "C"
