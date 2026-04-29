#pragma once

#include "ir/Ir.hpp"

#include <span>

namespace quartz::ir {

/// Intrinsics run by a shared runtime function (vm/Builtins.hpp) in both tiers.
inline bool isRuntimeBuiltin(Intrinsic op) {
    return (op >= Intrinsic::StrToInt && op <= Intrinsic::ListSort) || op == Intrinsic::ListSortByKeys;
}

inline constexpr std::uint32_t kRuntimeBuiltins =
    static_cast<std::uint32_t>(Intrinsic::ListSort) - static_cast<std::uint32_t>(Intrinsic::StrToInt) + 2;

inline std::uint32_t runtimeBuiltinIndex(Intrinsic op) {
    return op == Intrinsic::ListSortByKeys
               ? kRuntimeBuiltins - 1
               : static_cast<std::uint32_t>(op) - static_cast<std::uint32_t>(Intrinsic::StrToInt);
}

/// True for the intrinsics that may change a list's length, so counted loops over that list stay checked.
inline bool changesListLength(Intrinsic op) {
    return op == Intrinsic::ListPush || op == Intrinsic::ListPop || op == Intrinsic::ListClear ||
           op == Intrinsic::ListInsert || op == Intrinsic::ListRemoveAt || op == Intrinsic::ListInsertAll;
}

/// Element types `contains`, `indexOf` and `sort` compare: numbers, bool, char and str.
inline bool isComparableElement(const TypeTable& t, TypeId elem) {
    return t.isInteger(elem) || t.isFloat(elem) || elem == kBool || elem == kChar || elem == kStr;
}

/// Operand and result types of the intrinsics added after `Panic`; false when they do not match.
bool builtinSignatureOk(const TypeTable& t, Intrinsic op, std::span<const TypeId> args, TypeId dst);

} // namespace quartz::ir
