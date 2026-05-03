#pragma once

#include "vm/Value.hpp"

#include <cstdint>
#include <string_view>

namespace quartz::vm {

class Sandbox;

/// One fuel unit per this many bytes a runtime operation reads (spec 9.2, D34).
inline constexpr std::uint32_t kFuelByteShift = 4;
/// A byte written to new memory weighs this many read bytes: measured, filling fresh arena memory costs about that.
inline constexpr std::uint64_t kFuelWriteFactor = 4;

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
    static bool sortByKeys(Sandbox& sb, const Slot* a, Slot* out);

    // IntBuiltins.cpp: a, b and the type id of their integer type
    static bool checkedAdd(Sandbox& sb, const Slot* a, Slot* out);
    static bool checkedSub(Sandbox& sb, const Slot* a, Slot* out);
    static bool checkedMul(Sandbox& sb, const Slot* a, Slot* out);
    static bool checkedDiv(Sandbox& sb, const Slot* a, Slot* out);
    static bool saturatingAdd(Sandbox& sb, const Slot* a, Slot* out);
    static bool saturatingSub(Sandbox& sb, const Slot* a, Slot* out);
    static bool saturatingMul(Sandbox& sb, const Slot* a, Slot* out);
};

/// Offset of the first `needle` in `hay` or -1, the same answer as std::string_view::find (NativeRuntime.cpp).
std::int64_t findBytes(const char* hay, std::size_t hayLen, const char* needle, std::size_t needleLen);

/// Decodes one UTF-8 scalar at `i`; malformed input reads as U+FFFD for one byte.
std::uint32_t decodeUtf8(std::string_view s, std::size_t& i);

} // namespace quartz::vm
