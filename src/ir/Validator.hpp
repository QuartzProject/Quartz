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

/// Checks each user type's components and rejects types that contain themselves by value, nest too deeply or are too
/// large for a frame.
std::expected<void, ValidationError> validateTypeGraph(const TypeTable& types);

/// False when a dst, a or b field the opcode does not use is set; later passes count every set field.
bool operandsAllowed(const Inst& in);
/// Why a closure instruction (operands already checked) does not match its function; empty when it does.
std::string closureMismatch(const Module& m, const Function& fn, const Inst& in);
/// Why an array.get or array.set does not match its array, element and index types; empty when it does.
std::string arrayMismatch(const Module& m, const Function& fn, const Inst& in);
/// Imports, annotations, states and parameter counts: everything the function validator indexes without checking.
std::expected<void, ValidationError> validateTables(const Module& m);

} // namespace quartz::ir
