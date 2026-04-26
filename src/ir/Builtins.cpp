#include "ir/Builtins.hpp"

namespace quartz::ir {

bool builtinSignatureOk(const TypeTable& t, Intrinsic op, std::span<const TypeId> at, TypeId dst) {
    auto is = [&](std::initializer_list<TypeId> params, TypeId result) {
        if (at.size() != params.size() || dst != result) {
            return false;
        }
        std::size_t i = 0;
        for (TypeId p : params) {
            if (at[i++] != p) {
                return false;
            }
        }
        return true;
    };
    auto nullableOf = [&](TypeId inner) { return t.kind(dst) == TypeKind::Nullable && t.info(dst).elem == inner; };
    auto listOf = [&](TypeId x, TypeId elem) { return t.kind(x) == TypeKind::List && t.info(x).elem == elem; };
    bool list = !at.empty() && t.kind(at[0]) == TypeKind::List;
    TypeId elem = list ? t.info(at[0]).elem : kError;
    switch (op) {
    case Intrinsic::StrToInt:
        return at.size() == 1 && at[0] == kStr && nullableOf(kI64);
    case Intrinsic::StrToFloat:
        return at.size() == 1 && at[0] == kStr && nullableOf(kF64);
    case Intrinsic::StrReplace:
        return is({kStr, kStr, kStr}, kStr);
    case Intrinsic::StrSplit:
        return at.size() == 2 && at[0] == kStr && at[1] == kStr && listOf(dst, kStr);
    case Intrinsic::StrTrim:
    case Intrinsic::StrTrimStart:
    case Intrinsic::StrTrimEnd:
        return is({kStr}, kStr);
    case Intrinsic::StrRepeat:
        return is({kStr, kI64}, kStr);
    case Intrinsic::StrEqualsIgnoreCase:
        return is({kStr, kStr}, kBool);
    case Intrinsic::StrChars:
        return at.size() == 1 && at[0] == kStr && listOf(dst, kChar);
    case Intrinsic::CharFromInt:
        return at.size() == 1 && at[0] == kI64 && nullableOf(kChar);
    case Intrinsic::ListInsert:
        return list && is({at[0], kI64, elem}, kVoid);
    case Intrinsic::ListRemoveAt:
        return list && is({at[0], kI64}, elem);
    case Intrinsic::ListContains:
        return list && isComparableElement(t, elem) && is({at[0], elem}, kBool);
    case Intrinsic::ListIndexOf:
        return list && isComparableElement(t, elem) && at.size() == 2 && at[1] == elem && nullableOf(kI64);
    case Intrinsic::ListReverse:
        return list && is({at[0]}, kVoid);
    case Intrinsic::ListSlice:
        return list && is({at[0], kI64, kI64}, at[0]);
    case Intrinsic::ListInsertAll:
        return list && is({at[0], kI64, at[0]}, kVoid);
    case Intrinsic::ListSort:
        return list && isComparableElement(t, elem) && is({at[0]}, kVoid);
    case Intrinsic::CharCode:
        return is({kChar}, kI64);
    case Intrinsic::MathTan:
    case Intrinsic::MathAsin:
    case Intrinsic::MathAcos:
    case Intrinsic::MathAtan:
    case Intrinsic::MathExp:
    case Intrinsic::MathLog:
    case Intrinsic::MathLog10:
    case Intrinsic::MathRound:
        return is({kF64}, kF64);
    case Intrinsic::MathAtan2:
    case Intrinsic::MathFmod:
        return is({kF64, kF64}, kF64);
    case Intrinsic::MathClampI:
        return is({kI64, kI64, kI64}, kI64);
    case Intrinsic::MathClampF:
        return is({kF64, kF64, kF64}, kF64);
    default:
        return false;
    }
}

} // namespace quartz::ir
