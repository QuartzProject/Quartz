#include "Builtins.hpp"

#include "syntax/Token.hpp"

#include <format>

namespace quartz::lsp {

namespace {

constexpr BuiltinFunction kFunctions[] = {
    {"sqrt", "sqrt(x: f64) -> f64"},
    {"floor", "floor(x: f64) -> f64"},
    {"ceil", "ceil(x: f64) -> f64"},
    {"sin", "sin(x: f64) -> f64"},
    {"cos", "cos(x: f64) -> f64"},
    {"pow", "pow(x: f64, y: f64) -> f64"},
    {"abs", "abs<T>(x: T) -> T"},
    {"min", "min<T>(a: T, b: T) -> T"},
    {"max", "max<T>(a: T, b: T) -> T"},
    {"panic", "panic(message: str) -> !"},
    {"Ok", "Ok<T, E>(value: T) -> Result<T, E>"},
    {"Err", "Err<T, E>(error: E) -> Result<T, E>"},
    {"List", "List<T>() -> [T]"},
    {"Map", "Map<K, V>() -> Map<K, V>"},
};

constexpr std::string_view kTypes[] = {"void", "bool", "i8",  "i16",  "i32", "i64",   "u8",   "u16", "u32",
                                       "u64",  "f32",  "f64", "char", "str", "Bytes", "List", "Map", "Result"};

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
        add("len", "() -> i64");
        add("lower", "() -> str");
        add("upper", "() -> str");
        add("contains", "(text: str) -> bool");
        add("startsWith", "(prefix: str) -> bool");
        add("endsWith", "(suffix: str) -> bool");
        add("slice", "(begin: i64, end: i64) -> str");
        add("byteAt", "(index: i64) -> i64");
        add("find", "(text: str) -> i64?");
    } else if (info.kind == ir::TypeKind::List) {
        std::string elem = a.typeName(info.elem);
        add("len", "() -> i64");
        add("push", std::format("(value: {})", elem));
        add("pop", std::format("() -> {}?", elem));
        add("clear", "()");
        if (info.elem == ir::kStr) {
            add("join", "(separator: str) -> str");
        }
    } else if (info.kind == ir::TypeKind::Map) {
        std::string key = a.typeName(info.key);
        std::string value = a.typeName(info.elem);
        add("len", "() -> i64");
        add("get", std::format("(key: {}) -> {}?", key, value));
        add("set", std::format("(key: {}, value: {})", key, value));
        add("has", std::format("(key: {}) -> bool", key));
        add("remove", std::format("(key: {}) -> bool", key));
    } else if (recv == ir::kF64) {
        for (std::string_view n : {"sqrt", "floor", "ceil", "abs", "sin", "cos"}) {
            add(n, "() -> f64");
        }
    } else if (recv == ir::kI64) {
        add("abs", "() -> i64");
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
