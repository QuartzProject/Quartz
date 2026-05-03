#include "Builtins.hpp"

#include "ir/Builtins.hpp"
#include "syntax/Token.hpp"

#include <format>

namespace quartz::lsp {

namespace {

constexpr std::string_view kMath = "org.quartzlang.math";

constexpr BuiltinFunction kFunctions[] = {
    {"sqrt", "sqrt(x: float) -> float", kMath},
    {"floor", "floor(x: float) -> float", kMath},
    {"ceil", "ceil(x: float) -> float", kMath},
    {"sin", "sin(x: float) -> float", kMath},
    {"cos", "cos(x: float) -> float", kMath},
    {"pow", "pow(x: float, y: float) -> float", kMath},
    {"abs", "abs<T>(x: T) -> T", kMath},
    {"min", "min<T>(a: T, b: T) -> T", kMath},
    {"max", "max<T>(a: T, b: T) -> T", kMath},
    {"tan", "tan(x: float) -> float", kMath},
    {"asin", "asin(x: float) -> float", kMath},
    {"acos", "acos(x: float) -> float", kMath},
    {"atan", "atan(x: float) -> float", kMath},
    {"atan2", "atan2(y: float, x: float) -> float", kMath},
    {"exp", "exp(x: float) -> float", kMath},
    {"log", "log(x: float) -> float", kMath},
    {"log10", "log10(x: float) -> float", kMath},
    {"fmod", "fmod(x: float, y: float) -> float", kMath},
    {"round", "round(x: float) -> float", kMath},
    {"clamp", "clamp<T>(x: T, lo: T, hi: T) -> T", kMath},
    {"panic", "panic(message: str) -> !", {}},
    {"assert", "assert(condition: bool, message: str = \"assertion failed\")", {}},
    {"Ok", "Ok<T, E>(value: T) -> Result<T, E>", {}},
    {"Err", "Err<T, E>(error: E) -> Result<T, E>", {}},
    {"List", "List<T>() -> [T]", {}},
    {"Map", "Map<K, V>() -> Map<K, V>", {}},
};

constexpr std::string_view kTypes[] = {"void",  "bool", "int", "float",  "byte", "char", "str",
                                       "Bytes", "List", "Map", "Result", "i8",   "i16",  "i32",
                                       "i64",   "u8",   "u16", "u32",    "u64",  "f32",  "f64"};

#define QUARTZ_KW_TEXT(name, text) std::string_view(text),
constexpr std::string_view kKeywords[] = {QUARTZ_KEYWORDS(QUARTZ_KW_TEXT)};
#undef QUARTZ_KW_TEXT

} // namespace

std::vector<BuiltinMethod> builtinMethods(const Analysis& a, std::uint32_t recv) {
    std::vector<BuiltinMethod> out;
    const sema::Program* p = a.program();
    if (p == nullptr || recv == 0 || recv >= p->types.size()) {
        return out;
    }
    const ir::TypeInfo& info = p->types.info(recv);
    auto add = [&](std::string_view n, std::string sig) { out.push_back({std::string(n), std::string(n) + sig}); };
    if (recv == ir::kStr) {
        add("len", "() -> int");
        add("lower", "() -> str");
        add("upper", "() -> str");
        add("contains", "(text: str) -> bool");
        add("startsWith", "(prefix: str) -> bool");
        add("endsWith", "(suffix: str) -> bool");
        add("slice", "(begin: int, end: int) -> str");
        add("byteAt", "(index: int) -> int");
        add("find", "(text: str) -> int?");
        add("toInt", "() -> int?");
        add("toFloat", "() -> float?");
        add("replace", "(from: str, to: str) -> str");
        add("split", "(separator: str) -> [str]");
        add("trim", "() -> str");
        add("trimStart", "() -> str");
        add("trimEnd", "() -> str");
        add("repeat", "(count: int) -> str");
        add("equalsIgnoreCase", "(other: str) -> bool");
        add("chars", "() -> [char]");
    } else if (recv == ir::kChar) {
        add("code", "() -> int");
    } else if (info.kind == ir::TypeKind::List) {
        std::string elem = a.typeName(info.elem);
        add("len", "() -> int");
        add("push", std::format("(value: {})", elem));
        add("pop", std::format("() -> {}?", elem));
        add("clear", "()");
        if (info.elem == ir::kStr) {
            add("join", "(separator: str) -> str");
        }
        add("insert", std::format("(index: int, value: {})", elem));
        add("insertAll", std::format("(index: int, values: [{}])", elem));
        add("removeAt", std::format("(index: int) -> {}", elem));
        add("reverse", "()");
        add("slice", std::format("(begin: int, end: int) -> [{}]", elem));
        if (ir::isComparableElement(p->types, info.elem)) {
            add("contains", std::format("(value: {}) -> bool", elem));
            add("indexOf", std::format("(value: {}) -> int?", elem));
            add("sort", "()");
        }
        add("map", std::format("<U>(f: fn({}) -> U) -> [U]", elem));
        add("filter", std::format("(keep: fn({}) -> bool) -> [{}]", elem, elem));
        add("any", std::format("(test: fn({}) -> bool) -> bool", elem));
        add("all", std::format("(test: fn({}) -> bool) -> bool", elem));
        add("find", std::format("(test: fn({}) -> bool) -> {}?", elem, elem));
        add("indexWhere", std::format("(test: fn({}) -> bool) -> int?", elem));
        add("sortBy", std::format("<K>(key: fn({}) -> K)", elem));
        add("sortWith", std::format("(cmp: fn({}, {}) -> int)", elem, elem));
    } else if (info.kind == ir::TypeKind::Map) {
        std::string key = a.typeName(info.key);
        std::string value = a.typeName(info.elem);
        add("len", "() -> int");
        add("get", std::format("(key: {}) -> {}?", key, value));
        add("set", std::format("(key: {}, value: {})", key, value));
        add("has", std::format("(key: {}) -> bool", key));
        add("remove", std::format("(key: {}) -> bool", key));
    } else if (recv == ir::kF64) {
        for (std::string_view n : {"sqrt", "floor", "ceil", "abs", "sin", "cos", "tan", "asin", "acos", "atan", "exp",
                                   "log", "log10", "round"}) {
            add(n, "() -> float");
        }
        add("clamp", "(lo: float, hi: float) -> float");
    } else if (p->types.isInteger(recv)) {
        std::string t = a.typeName(recv);
        for (std::string_view n : {"checkedAdd", "checkedSub", "checkedMul", "checkedDiv"}) {
            add(n, std::format("(other: {}) -> {}?", t, t));
        }
        for (std::string_view n : {"saturatingAdd", "saturatingSub", "saturatingMul"}) {
            add(n, std::format("(other: {}) -> {}", t, t));
        }
        if (recv == ir::kI64) {
            add("abs", "() -> int");
            add("clamp", "(lo: int, hi: int) -> int");
            add("toChar", "() -> char?");
        }
    } else if (p->types.isResult(recv) && !info.variants.empty() && !info.variants[0].payload.empty()) {
        std::string inner = a.typeName(info.variants[0].payload[0]);
        add("isOk", "() -> bool");
        add("isErr", "() -> bool");
        add("unwrap", "() -> " + inner);
        add("unwrapOr", std::format("(fallback: {}) -> {}", inner, inner));
    } else if (info.kind == ir::TypeKind::Nullable) {
        std::string inner = a.typeName(info.elem);
        add("unwrap", "() -> " + inner);
        add("unwrapOr", std::format("(fallback: {}) -> {}", inner, inner));
    }
    return out;
}

std::optional<BuiltinMethod> findBuiltinMethod(const Analysis& a, std::uint32_t recv, std::string_view name) {
    for (BuiltinMethod& m : builtinMethods(a, recv)) {
        if (m.name == name) {
            return std::move(m);
        }
    }
    return std::nullopt;
}

std::span<const BuiltinFunction> builtinFunctions() { return kFunctions; }

const BuiltinFunction* findBuiltinFunction(std::string_view name) {
    for (const BuiltinFunction& f : kFunctions) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

std::span<const std::string_view> builtinTypes() { return kTypes; }

std::span<const std::string_view> keywords() { return kKeywords; }

} // namespace quartz::lsp
