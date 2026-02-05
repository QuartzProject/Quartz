#pragma once

#include "ir/Serialize.hpp"
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
};

struct CompileOutput {
    std::shared_ptr<const vm::Program> program; // null when compilation failed
    std::string diagnostics;                    // rendered errors and warnings
    std::string irText;                         // filled when requested
    std::vector<std::uint8_t> binary;           // the portable `.qzb` module, when requested
};

struct CompileOptions {
    bool printIr = false;
    bool optimize = true;
    bool emitBinary = false;
    bool binaryLines = true; // keep source lines for trap messages
    std::string manifest;    // `package.toml` text carried in the module
};

/// Front end, lowering, validation and bytecode generation in one call.
CompileOutput compile(std::span<const SourceInput> inputs, CompileOptions options = {});

/// Checks a `.qzb` module's hash and signatures, decodes and validates it, and generates its bytecode.
std::expected<std::shared_ptr<const vm::Program>, std::string> load(std::span<const std::uint8_t> bytes,
                                                                    const ir::ReadOptions& options = {});

} // namespace quartz
