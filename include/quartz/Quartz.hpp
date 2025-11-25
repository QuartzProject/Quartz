#pragma once

#include "vm/Sandbox.hpp"

#include <memory>
#include <span>
#include <string>

namespace quartz {

struct SourceInput {
    std::string name; // `.qzi` names are host interfaces
    std::string text;
};

struct CompileOutput {
    std::shared_ptr<const vm::Program> program; // null when compilation failed
    std::string diagnostics;                    // rendered errors and warnings
    std::string irText;                         // filled when requested
};

struct CompileOptions {
    bool printIr = false;
};

/// Front end, lowering, validation and bytecode generation in one call.
CompileOutput compile(std::span<const SourceInput> inputs, CompileOptions options = {});

} // namespace quartz
