#pragma once

#include "ir/Ir.hpp"

#include <expected>
#include <string>

namespace quartz::ir {

struct ValidationError {
    std::uint32_t function = ~0U;
    std::uint32_t inst = ~0U;
    std::string message;

    [[nodiscard]] std::string describe(const Module& m) const;
};

/// Checks everything the interpreter and the compiler rely on; never trusts the producer of the module.
std::expected<void, ValidationError> validate(const Module& m);

} // namespace quartz::ir
