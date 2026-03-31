#include "Builtins.hpp"

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
    {"panic", "panic(message: str) -> !", {}},
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
    } else if (info.kind == ir::TypeKind::List) {
        std::string elem = a.typeName(info.elem);
        add("len", "() -> int");
        add("push", std::format("(value: {})", elem));
        add("pop", std::format("() -> {}?", elem));
        add("clear", "()");
        if (info.elem == ir::kStr) {
            add("join", "(separator: str) -> str");
        }
    } else if (info.kind == ir::TypeKind::Map) {
        std::string key = a.typeName(info.key);
        std::string value = a.typeName(info.elem);
        add("len", "() -> int");
        add("get", std::format("(key: {}) -> {}?", key, value));
        add("set", std::format("(key: {}, value: {})", key, value));
        add("has", std::format("(key: {}) -> bool", key));
        add("remove", std::format("(key: {}) -> bool", key));
    } else if (recv == ir::kF64) {
        for (std::string_view n : {"sqrt", "floor", "ceil", "abs", "sin", "cos"}) {
            add(n, "() -> float");
        }
    } else if (recv == ir::kI64) {
        add("abs", "() -> int");
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
