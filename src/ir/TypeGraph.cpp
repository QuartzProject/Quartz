#include "ir/Validator.hpp"

#include <algorithm>
#include <format>

namespace quartz::ir {

namespace {

constexpr std::uint32_t kMaxTypeDepth = 64;
constexpr std::uint64_t kMaxValueSlots = 0xFFFF;

bool isNominal(TypeKind k) {
    return k == TypeKind::Struct || k == TypeKind::State || k == TypeKind::Enum || k == TypeKind::Handle;
}

bool pointerLike(TypeKind k) {
    switch (k) {
    case TypeKind::Str:
    case TypeKind::Bytes:
    case TypeKind::List:
    case TypeKind::Map:
    case TypeKind::State:
    case TypeKind::Fn:
    case TypeKind::Handle:
        return true;
    default:
        return false;
    }
}

/// Every recursive walk over types (names, layouts, state copies) stops at a reference; this proves they all end.
class TypeGraph {
public:
    explicit TypeGraph(const TypeTable& t) : t_(t), mark_(t.size(), 0), slots_(t.size(), 0) {}

    std::expected<void, ValidationError> run() {
        for (TypeId id = kFirstUserType; id < t_.size(); ++id) {
            if (!visit(id, 0)) {
                return std::unexpected(ValidationError{~0U, ~0U, std::format("type {}: {}", id, why_)});
            }
        }
        return {};
    }

private:
    bool child(TypeId c, bool viaRef, std::uint32_t depth, std::uint64_t& size) {
        if (c >= t_.size()) {
            why_ = "refers to an unknown type";
            return false;
        }
        if (viaRef && isNominal(t_.kind(c))) {
            return true;
        }
        if (!visit(c, depth + 1)) {
            return false;
        }
        size = std::min(size + slots_[c], kMaxValueSlots + 1);
        return true;
    }

    bool visit(TypeId id, std::uint32_t depth) {
        if (mark_[id] == 2) {
            return true;
        }
        if (mark_[id] == 1) {
            why_ = "contains itself by value";
            return false;
        }
        if (depth > kMaxTypeDepth) {
            why_ = "nested too deeply";
            return false;
        }
        mark_[id] = 1;
        const TypeInfo& info = t_.info(id);
        std::uint64_t size = 0;
        bool ok = true;
        switch (info.kind) {
        case TypeKind::List:
            ok = child(info.elem, true, depth, size);
            break;
        case TypeKind::Map:
            ok = child(info.key, true, depth, size) && child(info.elem, true, depth, size);
            break;
        case TypeKind::Fn:
            for (TypeId e : info.elems) {
                ok = ok && child(e, true, depth, size);
            }
            ok = ok && child(info.result, true, depth, size);
            break;
        case TypeKind::Nullable:
            ok = child(info.elem, false, depth, size);
            break;
        case TypeKind::Tuple:
            for (TypeId e : info.elems) {
                ok = ok && child(e, false, depth, size);
            }
            break;
        case TypeKind::Struct:
        case TypeKind::State:
            for (const Field& f : info.fields) {
                ok = ok && child(f.type, false, depth, size);
            }
            break;
        case TypeKind::Enum: {
            std::uint64_t largest = 0;
            for (const VariantInfo& v : info.variants) {
                std::uint64_t s = 0;
                for (TypeId p : v.payload) {
                    ok = ok && child(p, false, depth, s);
                }
                largest = std::max(largest, s);
            }
            size = largest;
            break;
        }
        default:
            break;
        }
        if (!ok) {
            return false;
        }
        switch (info.kind) {
        case TypeKind::Nullable:
            size = pointerLike(t_.kind(info.elem)) ? 1 : size + 1;
            break;
        case TypeKind::Struct:
        case TypeKind::Tuple:
            break;
        case TypeKind::Enum:
            size += 1;
            break;
        default:
            if (size > kMaxValueSlots) {
                why_ = "too large";
                return false;
            }
            size = info.kind == TypeKind::Void || info.kind == TypeKind::Never || info.kind == TypeKind::Error ? 0 : 1;
            break;
        }
        if (size > kMaxValueSlots) {
            why_ = "too large";
            return false;
        }
        slots_[id] = size;
        mark_[id] = 2;
        return true;
    }

    const TypeTable& t_;
    std::vector<std::uint8_t> mark_; // 0 unvisited, 1 on the current path, 2 done
    std::vector<std::uint64_t> slots_;
    std::string why_;
};

} // namespace

std::expected<void, ValidationError> validateTypeGraph(const TypeTable& types) { return TypeGraph(types).run(); }

} // namespace quartz::ir
