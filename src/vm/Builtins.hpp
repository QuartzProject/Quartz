#pragma once

#include "vm/Value.hpp"

#include <cstdint>
#include <string_view>

namespace quartz::vm {

class Sandbox;
struct List;

/// One fuel unit per this many bytes a runtime operation reads or writes (spec 9.2, D34).
inline constexpr std::uint32_t kFuelByteShift = 4;

/// The runtime builtins (ir::isRuntimeBuiltin): the interpreter and the native tier call the same code, so both give
/// the same bits and charge the same fuel. Arguments sit in consecutive slots in IR order; results go to `out`.
struct Builtins {
    using Fn = bool (*)(Sandbox& sb, const Slot* args, Slot* out);

    /// Runs builtin `index` (ir::runtimeBuiltinIndex); false once the sandbox trapped.
    static bool run(Sandbox& sb, std::uint32_t index, const Slot* args, Slot* out);
    /// Charges the fuel for touching `bytes` bytes; false once the budget is gone.
    static bool chargeBytes(Sandbox& sb, std::uint64_t bytes);

    // StrBuiltins.cpp
    static bool toInt(Sandbox& sb, const Slot* a, Slot* out);
    static bool toFloat(Sandbox& sb, const Slot* a, Slot* out);
    static bool replace(Sandbox& sb, const Slot* a, Slot* out);
    static bool split(Sandbox& sb, const Slot* a, Slot* out);
    static bool trim(Sandbox& sb, const Slot* a, Slot* out);
    static bool trimStart(Sandbox& sb, const Slot* a, Slot* out);
    static bool trimEnd(Sandbox& sb, const Slot* a, Slot* out);
    static bool repeat(Sandbox& sb, const Slot* a, Slot* out);
    static bool equalsIgnoreCase(Sandbox& sb, const Slot* a, Slot* out);
    static bool chars(Sandbox& sb, const Slot* a, Slot* out);
    static bool charFromInt(Sandbox& sb, const Slot* a, Slot* out);

    // ListBuiltins.cpp
    static bool insert(Sandbox& sb, const Slot* a, Slot* out);
    static bool removeAt(Sandbox& sb, const Slot* a, Slot* out);
    static bool contains(Sandbox& sb, const Slot* a, Slot* out);
    static bool indexOf(Sandbox& sb, const Slot* a, Slot* out);
    static bool reverse(Sandbox& sb, const Slot* a, Slot* out);
    static bool slice(Sandbox& sb, const Slot* a, Slot* out);
    static bool insertAll(Sandbox& sb, const Slot* a, Slot* out);
    static bool sort(Sandbox& sb, const Slot* a, Slot* out);

private:
    static bool makeRoom(Sandbox& sb, List* l, std::uint32_t at, std::uint32_t count);
};

/// Decodes one UTF-8 scalar at `i`; malformed input reads as U+FFFD for one byte.
std::uint32_t decodeUtf8(std::string_view s, std::size_t& i);

} // namespace quartz::vm
