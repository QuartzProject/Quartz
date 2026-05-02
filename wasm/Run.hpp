#pragma once

#include "vm/Sandbox.hpp"

#include <memory>
#include <string_view>

namespace quartz::wasm {

enum Stream : int { kPrint, kDiagnostics, kTrap, kStatus };

enum Outcome : int { kDone, kCompileError, kTrapped, kOutOfFuel, kNoEntry, kOutputLimit, kInvalidModule };

/// Hands one piece of output to `Module.onEmit(stream, text)` in JavaScript.
void emit(Stream stream, std::string_view text);

/// Runs `entry` as a task with the `quartz` tool's host functions; ticks pass at once, fuel slices are capped.
Outcome run(const std::shared_ptr<const vm::Program>& program, std::string_view entry, double fuelSlice,
            double maxSlices, double outputLimit);

} // namespace quartz::wasm
