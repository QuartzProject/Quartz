#include "ir/Builtins.hpp"

namespace quartz::ir {

bool languageIntrinsicOk(const TypeTable& t, Intrinsic op, std::span<const TypeId> at, TypeId dst) {
    bool overMap = at.size() == 2 && t.kind(at[0]) == TypeKind::Map && at[1] == kI64;
    switch (op) {
    case Intrinsic::MapNext:
        return overMap && dst == kI64;
    case Intrinsic::MapKeyAt:
        return overMap && dst == t.info(at[0]).key;
    case Intrinsic::MapValueAt:
        return overMap && dst == t.info(at[0]).elem;
    case Intrinsic::StrDecode:
        return at.size() == 2 && at[0] == kStr && at[1] == kI64 && t.kind(dst) == TypeKind::Tuple &&
               t.info(dst).elems == std::vector<TypeId>{kChar, kI64};
    case Intrinsic::StrFormat:
        return at.size() == 2 && (at[0] == kI64 || at[0] == kU64 || at[0] == kF64 || at[0] == kStr) && at[1] == kI64 &&
               dst == kStr;
    default:
        return false;
    }
}

} // namespace quartz::ir
