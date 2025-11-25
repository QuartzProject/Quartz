#include "ir/Types.hpp"

#include <format>

namespace quartz::ir {

TypeTable::TypeTable() {
    const std::pair<TypeKind, const char*> builtins[] = {
        {TypeKind::Error, "<error>"}, {TypeKind::Void, "void"}, {TypeKind::Bool, "bool"}, {TypeKind::I8, "i8"},
        {TypeKind::I16, "i16"},       {TypeKind::I32, "i32"},   {TypeKind::I64, "i64"},   {TypeKind::U8, "u8"},
        {TypeKind::U16, "u16"},       {TypeKind::U32, "u32"},   {TypeKind::U64, "u64"},   {TypeKind::F32, "f32"},
        {TypeKind::F64, "f64"},       {TypeKind::Char, "char"}, {TypeKind::Str, "str"},   {TypeKind::Bytes, "Bytes"},
        {TypeKind::Null, "null"},    {TypeKind::Never, "never"},
    };
    for (const auto& [kind, name] : builtins) {
        TypeInfo t;
        t.kind = kind;
        t.name = name;
        types_.push_back(std::move(t));
    }
}

TypeId TypeTable::intern(TypeInfo info, const std::string& key) {
    auto it = structural_.find(key);
    if (it != structural_.end()) {
        return it->second;
    }
    auto id = static_cast<TypeId>(types_.size());
    types_.push_back(std::move(info));
    structural_.emplace(key, id);
    return id;
}

TypeId TypeTable::list(TypeId elem) {
    TypeInfo t;
    t.kind = TypeKind::List;
    t.elem = elem;
    return intern(std::move(t), std::format("L{}", elem));
}

TypeId TypeTable::map(TypeId key, TypeId value) {
    TypeInfo t;
    t.kind = TypeKind::Map;
    t.key = key;
    t.elem = value;
    return intern(std::move(t), std::format("M{},{}", key, value));
}

TypeId TypeTable::nullable(TypeId inner) {
    if (kind(inner) == TypeKind::Nullable || inner == kError) {
        return inner;
    }
    TypeInfo t;
    t.kind = TypeKind::Nullable;
    t.elem = inner;
    return intern(std::move(t), std::format("N{}", inner));
}

TypeId TypeTable::tuple(const std::vector<TypeId>& elems) {
    std::string key = "T";
    for (TypeId e : elems) {
        key += std::format("{},", e);
    }
    TypeInfo t;
    t.kind = TypeKind::Tuple;
    t.elems = elems;
    return intern(std::move(t), key);
}

TypeId TypeTable::fn(const std::vector<TypeId>& params, TypeId result) {
    std::string key = "F";
    for (TypeId p : params) {
        key += std::format("{},", p);
    }
    key += std::format("->{}", result);
    TypeInfo t;
    t.kind = TypeKind::Fn;
    t.elems = params;
    t.result = result;
    return intern(std::move(t), key);
}

TypeId TypeTable::declareNominal(TypeKind k, std::string name) {
    TypeInfo t;
    t.kind = k;
    t.name = std::move(name);
    t.complete = false;
    auto id = static_cast<TypeId>(types_.size());
    types_.push_back(std::move(t));
    return id;
}

TypeId TypeTable::addRaw(TypeInfo info) {
    switch (info.kind) {
    case TypeKind::List: return list(info.elem);
    case TypeKind::Map: return map(info.key, info.elem);
    case TypeKind::Nullable: return nullable(info.elem);
    case TypeKind::Tuple: return tuple(info.elems);
    case TypeKind::Fn: return fn(info.elems, info.result);
    default: break;
    }
    auto id = static_cast<TypeId>(types_.size());
    types_.push_back(std::move(info));
    return id;
}

std::string TypeTable::name(TypeId id) const {
    if (id >= types_.size()) {
        return "<invalid>";
    }
    const TypeInfo& t = types_[id];
    switch (t.kind) {
    case TypeKind::List: return std::format("[{}]", name(t.elem));
    case TypeKind::Map: return std::format("Map<{}, {}>", name(t.key), name(t.elem));
    case TypeKind::Nullable: return std::format("{}?", name(t.elem));
    case TypeKind::Tuple:
    case TypeKind::Fn: {
        std::string out = "(";
        for (std::size_t i = 0; i < t.elems.size(); ++i) {
            out += (i ? ", " : "") + name(t.elems[i]);
        }
        out += ")";
        if (t.kind == TypeKind::Fn) {
            out += " -> " + name(t.result);
        }
        return out;
    }
    default: return t.name;
    }
}

bool TypeTable::isInteger(TypeId id) const { return id >= kI8 && id <= kU64; }

bool TypeTable::isSigned(TypeId id) const { return id >= kI8 && id <= kI64; }

int TypeTable::bitWidth(TypeId id) const {
    switch (id) {
    case kI8:
    case kU8: return 8;
    case kI16:
    case kU16: return 16;
    case kI32:
    case kU32:
    case kF32:
    case kChar: return 32;
    case kBool: return 1;
    default: return 64;
    }
}

bool TypeTable::isRefKind(TypeId id) const {
    switch (kind(id)) {
    case TypeKind::Str:
    case TypeKind::Bytes:
    case TypeKind::List:
    case TypeKind::Map:
    case TypeKind::State:
    case TypeKind::Fn: return true;
    default: return false;
    }
}

bool TypeTable::hasRefs(TypeId id) const {
    const TypeInfo& t = types_[id];
    switch (t.kind) {
    case TypeKind::Nullable: return hasRefs(t.elem);
    case TypeKind::Tuple:
        for (TypeId e : t.elems) {
            if (hasRefs(e)) {
                return true;
            }
        }
        return false;
    case TypeKind::Struct:
        for (const Field& f : t.fields) {
            if (hasRefs(f.type)) {
                return true;
            }
        }
        return false;
    case TypeKind::Enum:
        for (const VariantInfo& v : t.variants) {
            for (TypeId p : v.payload) {
                if (hasRefs(p)) {
                    return true;
                }
            }
        }
        return false;
    default: return isRefKind(id);
    }
}

} // namespace quartz::ir
