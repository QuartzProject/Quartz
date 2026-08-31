#pragma once

#include "ir/Serialize.hpp"
#include "support/Diagnostics.hpp"
#include "vm/Sandbox.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quartz {

struct SourceInput {
    std::string name; // `.qzi` names are host interfaces
    std::string text;
    bool dependency = false; // a library compiled in from the registry: its `@test` functions are not this package's
};

/// One error, warning or note with its place; lines and columns start at 1 and columns count bytes.
struct CompileMessage {
    Severity severity = Severity::Error;
    std::string rule; // the warning rule (`unused-variable`), also on a warning raised to an error; empty otherwise
    std::string file; // empty, and the place 0, when it belongs to no source
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t endLine = 0;
    std::uint32_t endColumn = 0; // one past the last byte
    std::string message;
};

struct CompileOutput {
    std::shared_ptr<const vm::Program> program; // null when compilation failed
    std::string diagnostics;                    // rendered errors and warnings
    std::size_t warnings = 0;
    std::vector<CompileMessage> messages;       // the same diagnostics, one by one
    std::string irText;                         // filled when requested
    std::vector<std::uint8_t> binary;           // the portable `.qzb` module, when requested
};

struct CompileOptions {
    bool printIr = false;
    bool optimize = true;
    bool emitBinary = false;
    bool binaryLines = true; // keep source lines for trap messages
    vm::Probes probes{};     // debugger and profiler hooks; line probes also skip the IR optimizer
    std::string manifest;    // `package.toml` text carried in the module
    bool lintLibrary = false; // also warn about the standard library sources, to keep them clean
    std::vector<RuleSetting> rules{}; // warning levels by rule id (`*` for all); a warning raised to error fails
};

/// Front end, lowering, validation and bytecode generation in one call.
CompileOutput compile(std::span<const SourceInput> inputs, CompileOptions options = {});

/// Checks a `.qzb` module's hash and signatures, decodes and validates it, and generates its bytecode.
std::expected<std::shared_ptr<const vm::Program>, std::string>
load(std::span<const std::uint8_t> bytes, const ir::ReadOptions& options = {}, vm::Probes probes = {});

} // namespace quartz
