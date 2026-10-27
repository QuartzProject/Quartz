#include "ir/Types.hpp"

#include <algorithm>

namespace quartz::ir {

namespace {

TypeId familyOf(const TypeTable& t, TypeId id) {
    TypeId base = t.info(id).genericBase;
    return base != 0 ? base : id;
}

/// The types a value holds by value: what a walk over inline storage visits next.
template <class Visit>
void inlineChildren(const TypeTable& t, TypeId id, bool enumsToo, const Visit& visit) {
    const TypeInfo& info = t.info(id);
    switch (info.kind) {
    case TypeKind::Nullable:
        visit(info.elem);
        break;
    case TypeKind::Tuple:
        for (TypeId e : info.elems) {
            visit(e);
        }
        break;
    case TypeKind::Struct:
        for (const Field& f : info.fields) {
            visit(f.type);
        }
        break;
    case TypeKind::Enum:
        if (enumsToo || t.isResult(id)) {
            for (const VariantInfo& v : info.variants) {
                for (TypeId p : v.payload) {
                    visit(p);
                }
            }
        }
        break;
    default:
        break;
    }
}

// Iterative so a long chain of declarations cannot exhaust the native stack.
template <class Hit>
bool reaches(const TypeTable& t, TypeId from, bool enumsToo, const Hit& hit) {
    std::vector<bool> seen(t.size(), false);
    std::vector<TypeId> work{from};
    while (!work.empty()) {
        TypeId id = work.back();
        work.pop_back();
        if (id >= t.size() || seen[id]) {
            continue;
        }
        seen[id] = true;
        if (hit(id)) {
            return true;
        }
        inlineChildren(t, id, enumsToo, [&](TypeId c) { work.push_back(c); });
    }
    return false;
}

} // namespace

std::vector<std::vector<bool>> TypeTable::recursivePayloads(TypeId e) const {
    const TypeInfo& info = types_[e];
    std::vector<std::vector<bool>> out(info.variants.size());
    if (info.kind != TypeKind::Enum || isResult(e)) {
        for (std::size_t v = 0; v < info.variants.size(); ++v) {
            out[v].assign(info.variants[v].payload.size(), false);
        }
        return out;
    }
    TypeId family = familyOf(*this, e);
    for (std::size_t v = 0; v < info.variants.size(); ++v) {
        for (TypeId p : info.variants[v].payload) {
            out[v].push_back(reaches(*this, p, true, [&](TypeId id) {
                TypeKind k = types_[id].kind;
                return (k == TypeKind::Enum || k == TypeKind::Struct) && familyOf(*this, id) == family;
            }));
        }
    }
    return out;
}

void TypeTable::boxRecursivePayloads() {
    std::vector<std::pair<TypeId, std::vector<std::vector<bool>>>> boxes;
    for (TypeId id = kFirstUserType; id < types_.size(); ++id) {
        if (types_[id].kind != TypeKind::Enum || isResult(id)) {
            continue;
        }
        std::vector<std::vector<bool>> b = recursivePayloads(id);
        for (const std::vector<bool>& v : b) {
            if (std::find(v.begin(), v.end(), true) != v.end()) {
                boxes.emplace_back(id, std::move(b));
                break;
            }
        }
    }
    for (auto& [id, b] : boxes) {
        for (std::size_t v = 0; v < b.size(); ++v) {
            for (std::size_t i = 0; i < b[v].size(); ++i) {
                if (b[v][i]) {
                    TypeId boxed = list(types_[id].variants[v].payload[i]);
                    VariantInfo& var = types_[id].variants[v];
                    var.payload[i] = boxed;
                    var.boxed.resize(var.payload.size());
                    var.boxed[i] = true;
                }
            }
        }
    }
}

std::optional<std::size_t> TypeTable::inlineCycle(TypeId id) const {
    const TypeInfo& info = types_[id];
    if (info.kind != TypeKind::Struct) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < info.fields.size(); ++i) {
        if (reaches(*this, info.fields[i].type, false, [&](TypeId x) { return x == id; })) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace quartz::ir
