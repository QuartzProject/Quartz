#include "ir/Builtins.hpp"
#include "sema/SemaImpl.hpp"

#include <algorithm>
#include <format>

namespace quartz::sema {

using ir::Intrinsic;

namespace {

constexpr std::pair<std::string_view, Intrinsic> kUnaryFloat[] = {
    {"tan", Intrinsic::MathTan},     {"asin", Intrinsic::MathAsin},   {"acos", Intrinsic::MathAcos},
    {"atan", Intrinsic::MathAtan},   {"exp", Intrinsic::MathExp},     {"log", Intrinsic::MathLog},
    {"log10", Intrinsic::MathLog10}, {"round", Intrinsic::MathRound},
};

} // namespace

bool isExtraMathFunction(std::string_view name) {
    static constexpr std::string_view kNames[] = {"tan", "asin",  "acos", "atan",  "atan2", "exp",
                                                  "log", "log10", "fmod", "round", "clamp"};
    return std::find(std::begin(kNames), std::end(kNames), name) != std::end(kNames);
}

std::optional<BuiltinSig> Checker::extraBuiltinMethod(TypeId recv, std::string_view name) {
    const ir::TypeInfo& info = types_->info(recv);
    if (recv == ir::kStr) {
        static const std::pair<std::string_view, BuiltinSig> table[] = {
            {"replace", {Intrinsic::StrReplace, {ir::kStr, ir::kStr}, ir::kStr}},
            {"trim", {Intrinsic::StrTrim, {}, ir::kStr}},
            {"trimStart", {Intrinsic::StrTrimStart, {}, ir::kStr}},
            {"trimEnd", {Intrinsic::StrTrimEnd, {}, ir::kStr}},
            {"repeat", {Intrinsic::StrRepeat, {ir::kI64}, ir::kStr}},
            {"equalsIgnoreCase", {Intrinsic::StrEqualsIgnoreCase, {ir::kStr}, ir::kBool}},
        };
        for (const auto& [n, sig] : table) {
            if (n == name) {
                return sig;
            }
        }
        if (name == "toInt") {
            return BuiltinSig{Intrinsic::StrToInt, {}, types_->nullable(ir::kI64)};
        }
        if (name == "toFloat") {
            return BuiltinSig{Intrinsic::StrToFloat, {}, types_->nullable(ir::kF64)};
        }
        if (name == "split") {
            return BuiltinSig{Intrinsic::StrSplit, {ir::kStr}, types_->list(ir::kStr)};
        }
        if (name == "chars") {
            return BuiltinSig{Intrinsic::StrChars, {}, types_->list(ir::kChar)};
        }
        return std::nullopt;
    }
    if (recv == ir::kChar) {
        if (name == "code") {
            return BuiltinSig{Intrinsic::CharCode, {}, ir::kI64};
        }
        return std::nullopt;
    }
    if (info.kind == ir::TypeKind::List) {
        TypeId elem = info.elem;
        bool comparable = ir::isComparableElement(*types_, elem);
        if (name == "insert") {
            return BuiltinSig{Intrinsic::ListInsert, {ir::kI64, elem}, ir::kVoid};
        }
        if (name == "removeAt") {
            return BuiltinSig{Intrinsic::ListRemoveAt, {ir::kI64}, elem};
        }
        if (name == "reverse") {
            return BuiltinSig{Intrinsic::ListReverse, {}, ir::kVoid};
        }
        if (name == "slice") {
            return BuiltinSig{Intrinsic::ListSlice, {ir::kI64, ir::kI64}, recv};
        }
        if (name == "insertAll") {
            return BuiltinSig{Intrinsic::ListInsertAll, {ir::kI64, recv}, ir::kVoid};
        }
        if (name == "contains" && comparable) {
            return BuiltinSig{Intrinsic::ListContains, {elem}, ir::kBool};
        }
        if (name == "indexOf" && comparable) {
            return BuiltinSig{Intrinsic::ListIndexOf, {elem}, types_->nullable(ir::kI64)};
        }
        if (name == "sort" && comparable) {
            return BuiltinSig{Intrinsic::ListSort, {}, ir::kVoid};
        }
        return std::nullopt;
    }
    if (recv == ir::kF64) {
        for (const auto& [n, op] : kUnaryFloat) {
            if (n == name) {
                return BuiltinSig{op, {}, ir::kF64};
            }
        }
        if (name == "clamp") {
            return BuiltinSig{Intrinsic::MathClampF, {ir::kF64, ir::kF64}, ir::kF64};
        }
        return std::nullopt;
    }
    if (recv == ir::kI64) {
        if (name == "clamp") {
            return BuiltinSig{Intrinsic::MathClampI, {ir::kI64, ir::kI64}, ir::kI64};
        }
        if (name == "toChar") {
            return BuiltinSig{Intrinsic::CharFromInt, {}, types_->nullable(ir::kChar)};
        }
    }
    return std::nullopt;
}

bool Checker::checkExtraBuiltinFunction(CallExpr* call, std::string_view n, TypeId expected, TypeId& result) {
    auto builtin = [&](Intrinsic op, std::vector<TypeId> params, TypeId r) {
        call->callKind = CallKind::Builtin;
        call->builtin = static_cast<std::uint16_t>(op);
        bindArgs(call, params, {}, {}, 0);
        result = r;
        return true;
    };
    for (const auto& [name, op] : kUnaryFloat) {
        if (name == n) {
            return builtin(op, {ir::kF64}, ir::kF64);
        }
    }
    if (n == "atan2") {
        return builtin(Intrinsic::MathAtan2, {ir::kF64, ir::kF64}, ir::kF64);
    }
    if (n == "fmod") {
        return builtin(Intrinsic::MathFmod, {ir::kF64, ir::kF64}, ir::kF64);
    }
    if (n == "clamp") {
        if (call->args.size() != 3) {
            diags_.error(call->span, "'clamp' takes 3 argument(s)");
            result = ir::kError;
            return true;
        }
        TypeId t = checkValue(call->args[0].value, expected);
        if (t != ir::kI64 && t != ir::kF64 && t != ir::kError) {
            diags_.error(call->span, "'clamp' works on int and float");
            result = ir::kError;
            return true;
        }
        return builtin(t == ir::kF64 ? Intrinsic::MathClampF : Intrinsic::MathClampI, {t, t, t}, t);
    }
    if (n == "assert") {
        call->callKind = CallKind::Special;
        call->builtin = static_cast<std::uint16_t>(SpecialOp::Assert);
        std::vector<TypeId> params{ir::kBool};
        if (call->args.size() > 1) {
            params.push_back(ir::kStr);
        }
        bindArgs(call, params, {}, {}, 0);
        result = ir::kVoid;
        return true;
    }
    return false;
}

} // namespace quartz::sema
