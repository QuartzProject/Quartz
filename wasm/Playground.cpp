#include "HostInterface.hpp"
#include "Run.hpp"
#include "quartz/Quartz.hpp"

#include <emscripten/emscripten.h>
#include <string>
#include <vector>

namespace {

std::vector<quartz::SourceInput> gSources;
std::vector<std::uint8_t> gModule;

quartz::CompileOutput compileSources(bool binary) {
    std::vector<quartz::SourceInput> inputs = gSources;
    inputs.push_back({std::string(quartz::cli::kHostInterfaceName), std::string(quartz::cli::kHostInterface)});
    quartz::CompileOutput out = quartz::compile(inputs, {.emitBinary = binary, .manifest = {}});
    if (!out.diagnostics.empty()) {
        quartz::wasm::emit(quartz::wasm::kDiagnostics, out.diagnostics);
    }
    return out;
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void qzp_reset() { gSources.clear(); }

/// Adds a source file (`.qz`, or `.qzi` for a host interface) to the next compilation.
EMSCRIPTEN_KEEPALIVE void qzp_source(const char* name, const char* text) { gSources.push_back({name, text}); }

EMSCRIPTEN_KEEPALIVE int qzp_run(const char* entry, double fuelSlice, double maxSlices, double outputLimit) {
    quartz::CompileOutput out = compileSources(false);
    if (!out.program) {
        return quartz::wasm::kCompileError;
    }
    return quartz::wasm::run(out.program, entry, fuelSlice, maxSlices, outputLimit);
}

/// Compiles to a `.qzb` module, readable with `qzp_module_data` until the next build; its size, or -1.
EMSCRIPTEN_KEEPALIVE int qzp_build() {
    quartz::CompileOutput out = compileSources(true);
    gModule = std::move(out.binary);
    return out.program ? static_cast<int>(gModule.size()) : -1;
}

EMSCRIPTEN_KEEPALIVE const std::uint8_t* qzp_module_data() { return gModule.data(); }

} // extern "C"
