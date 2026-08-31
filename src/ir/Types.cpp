#include "ir/Types.hpp"

#include <algorithm>
#include <format>

namespace quartz::ir {

TypeTable::TypeTable() {
    const std::pair<TypeKind, const char*> builtins[] = {
        {TypeKind::Error, "<error>"}, {TypeKind::Void, "void"}, {TypeKind::Bool, "bool"}, {TypeKind::I8, "i8"},
        {TypeKind::I16, "i16"},       {TypeKind::I32, "i32"},   {TypeKind::I64, "int"},   {TypeKind::U8, "byte"},
        {TypeKind::U16, "u16"},       {TypeKind::U32, "u32"},   {TypeKind::U64, "u64"},   {TypeKind::F32, "f32"},
        {TypeKind::F64, "float"},       {TypeKind::Char, "char"}, {TypeKind::Str, "str"},   {TypeKind::Bytes, "Bytes"},
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

TypeId TypeTable::array(TypeId elem, std::uint32_t n) {
    TypeInfo t;
    t.kind = TypeKind::Tuple;
    t.array = true;
    t.elems.assign(n, elem);
    return intern(std::move(t), std::format("A{},{}", elem, n));
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

TypeId TypeTable::result(TypeId ok, TypeId err) {
    TypeInfo t;
    t.kind = TypeKind::Enum;
    t.name = std::format("Result<{}, {}>", name(ok), name(err));
    t.variants.push_back({"Ok", {ok}});
    t.variants.push_back({"Err", {err}});
    return intern(std::move(t), std::format("R{},{}", ok, err));
}

TypeId TypeTable::param(std::string name) {
    TypeInfo t;
    t.kind = TypeKind::Param;
    t.name = std::move(name);
    auto id = static_cast<TypeId>(types_.size());
    types_.push_back(std::move(t));
    return id;
}

TypeId TypeTable::instantiate(TypeId base, const std::vector<TypeId>& args) {
    auto key = std::make_pair(base, args);
    auto it = instances_.find(key);
    if (it != instances_.end()) {
        return it->second;
    }
    std::string name = types_[base].name + "<";
    for (std::size_t i = 0; i < args.size(); ++i) {
        name += (i ? ", " : "") + this->name(args[i]);
    }
    name += ">";
    TypeInfo t;
    t.kind = types_[base].kind;
    t.name = std::move(name);
    t.genericBase = base;
    t.typeArgs = args;
    t.package = types_[base].package;
    auto id = static_cast<TypeId>(types_.size());
    types_.push_back(std::move(t));
    instances_.emplace(key, id);
    TypeSubst map;
    for (std::size_t i = 0; i < args.size() && i < types_[base].typeParams.size(); ++i) {
        map.emplace_back(types_[base].typeParams[i], args[i]);
    }
    std::vector<Field> fields;
    for (const Field& f : std::vector<Field>(types_[base].fields)) {
        fields.push_back({f.name, subst(f.type, map), f.renamedFrom});
    }
    std::vector<VariantInfo> variants;
    for (const VariantInfo& v : std::vector<VariantInfo>(types_[base].variants)) {
        VariantInfo nv{v.name, {}};
        for (TypeId p : v.payload) {
            nv.payload.push_back(subst(p, map));
        }
        variants.push_back(std::move(nv));
    }
    types_[id].fields = std::move(fields);
    types_[id].variants = std::move(variants);
    return id;
}

void TypeTable::refreshInstances() {
    for (TypeId id = 0; id < types_.size(); ++id) {
        if (types_[id].genericBase == 0) {
            continue;
        }
        TypeId base = types_[id].genericBase;
        std::vector<TypeId> args = types_[id].typeArgs;
        TypeSubst map;
        for (std::size_t i = 0; i < args.size() && i < types_[base].typeParams.size(); ++i) {
            map.emplace_back(types_[base].typeParams[i], args[i]);
        }
        std::vector<Field> fields;
        for (const Field& f : std::vector<Field>(types_[base].fields)) {
            fields.push_back({f.name, subst(f.type, map), f.renamedFrom});
        }
        std::vector<VariantInfo> variants;
        for (const VariantInfo& v : std::vector<VariantInfo>(types_[base].variants)) {
            VariantInfo nv{v.name, {}};
            for (TypeId p : v.payload) {
                nv.payload.push_back(subst(p, map));
            }
            variants.push_back(std::move(nv));
        }
        types_[id].fields = std::move(fields);
        types_[id].variants = std::move(variants);
    }
}

TypeId TypeTable::subst(TypeId t, const TypeSubst& map) {
    if (map.empty() || !hasParams(t)) {
        return t;
    }
    TypeInfo info = types_[t];
    switch (info.kind) {
    case TypeKind::Param:
        for (const auto& [from, to] : map) {
            if (from == t) {
                return to;
            }
        }
        return t;
    case TypeKind::List: return list(subst(info.elem, map));
    case TypeKind::Map: return this->map(subst(info.key, map), subst(info.elem, map));
    case TypeKind::Nullable: return nullable(subst(info.elem, map));
    case TypeKind::Tuple:
    case TypeKind::Fn: {
        std::vector<TypeId> elems;
        for (TypeId e : info.elems) {
            elems.push_back(subst(e, map));
        }
        if (info.array) {
            return array(elems[0], static_cast<std::uint32_t>(elems.size()));
        }
        return info.kind == TypeKind::Tuple ? tuple(elems) : fn(elems, subst(info.result, map));
    }
    default: break;
    }
    if (isResult(t)) {
        return result(subst(info.variants[0].payload[0], map), subst(info.variants[1].payload[0], map));
    }
    if (info.genericBase != 0) {
        std::vector<TypeId> args;
        for (TypeId a : info.typeArgs) {
            args.push_back(subst(a, map));
        }
        return instantiate(info.genericBase, args);
    }
    return t;
}

bool TypeTable::hasParams(TypeId t) const {
    const TypeInfo& info = types_[t];
    switch (info.kind) {
    case TypeKind::Param: return true;
    case TypeKind::List:
    case TypeKind::Nullable: return hasParams(info.elem);
    case TypeKind::Map: return hasParams(info.key) || hasParams(info.elem);
    case TypeKind::Tuple:
    case TypeKind::Fn:
        for (TypeId e : info.elems) {
            if (hasParams(e)) {
                return true;
            }
        }
        return info.kind == TypeKind::Fn && hasParams(info.result);
    default: break;
    }
    if (isResult(t)) {
        return hasParams(info.variants[0].payload[0]) || hasParams(info.variants[1].payload[0]);
    }
    for (TypeId a : info.typeArgs) {
        if (hasParams(a)) {
            return true;
        }
    }
    return false;
}

bool TypeTable::isResult(TypeId id) const {
    const TypeInfo& t = types_[id];
    return t.kind == TypeKind::Enum && t.variants.size() == 2 && t.variants[0].name == "Ok" &&
           t.variants[1].name == "Err" && t.name.starts_with("Result<");
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
    case TypeKind::Tuple:
        return info.array && !info.elems.empty() ? array(info.elems[0], static_cast<std::uint32_t>(info.elems.size()))
                                                 : tuple(info.elems);
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
        if (t.array && !t.elems.empty()) {
            return std::format("[{}; {}]", name(t.elems[0]), t.elems.size());
        }
        [[fallthrough]];
    case TypeKind::Fn: {
        std::string out = t.kind == TypeKind::Fn ? "fn(" : "(";
        for (std::size_t i = 0; i < t.elems.size(); ++i) {
            out += (i ? ", " : "") + name(t.elems[i]);
        }
        out += ")";
        if (t.kind == TypeKind::Fn && t.result != kVoid) {
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

namespace {

template <class Bad>
bool containsFrom(const TypeTable& t, TypeId id, std::vector<bool>& seen, const Bad& bad) {
    if (id >= t.size() || seen[id]) {
        return false;
    }
    seen[id] = true;
    const TypeInfo& info = t.info(id);
    if (bad(info.kind)) {
        return true;
    }
    auto any = [&](const std::vector<TypeId>& ids) {
        return std::any_of(ids.begin(), ids.end(), [&](TypeId x) { return containsFrom(t, x, seen, bad); });
    };
    switch (info.kind) {
    case TypeKind::List:
    case TypeKind::Nullable:
        return containsFrom(t, info.elem, seen, bad);
    case TypeKind::Map:
        return containsFrom(t, info.key, seen, bad) || containsFrom(t, info.elem, seen, bad);
    case TypeKind::Tuple:
        return any(info.elems);
    case TypeKind::Struct:
    case TypeKind::State:
        return std::any_of(info.fields.begin(), info.fields.end(),
                           [&](const Field& f) { return containsFrom(t, f.type, seen, bad); });
    case TypeKind::Enum:
        return std::any_of(info.variants.begin(), info.variants.end(),
                           [&](const VariantInfo& v) { return any(v.payload); });
    default:
        return false;
    }
}

} // namespace

bool TypeTable::containsFn(TypeId id) const {
    std::vector<bool> seen(types_.size(), false);
    return containsFrom(*this, id, seen, [](TypeKind k) { return k == TypeKind::Fn; });
}

bool TypeTable::hostBuildable(TypeId id) const {
    if (id >= types_.size() || id == kVoid || id == kError || id == kNull || id == kNever) {
        return false;
    }
    std::vector<bool> seen(types_.size(), false);
    return !containsFrom(*this, id, seen,
                         [](TypeKind k) { return k == TypeKind::Fn || k == TypeKind::State || k == TypeKind::Param; });
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
