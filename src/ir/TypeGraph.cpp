#include "ir/Validator.hpp"

#include <algorithm>
#include <format>

namespace quartz::ir {

namespace {

constexpr std::uint64_t kMaxValueSlots = 0xFFFF;

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
        if (viaRef) {
            return true; // one slot whatever it points to; `run` checks the target as a root of its own
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
        if (depth > kMaxTypeNesting) {
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

std::expected<void, ValidationError> validateTypes(const TypeTable& t) {
    for (TypeId id = kFirstUserType; id < t.size(); ++id) {
        const TypeInfo& info = t.info(id);
        auto ok = [&](TypeId x) { return x < t.size() && x != kVoid && x != kError && x != kNull && x != kNever; };
        bool good = true;
        switch (info.kind) {
        case TypeKind::List:
        case TypeKind::Nullable:
            good = ok(info.elem);
            break;
        case TypeKind::Map:
            good = ok(info.key) && ok(info.elem);
            break;
        case TypeKind::Tuple:
            for (TypeId e : info.elems) {
                good = good && ok(e);
            }
            break;
        case TypeKind::Struct:
        case TypeKind::State:
            for (const Field& f : info.fields) {
                good = good && ok(f.type) && !(info.kind == TypeKind::Struct && f.type == id);
            }
            break;
        case TypeKind::Enum:
            for (const VariantInfo& v : info.variants) {
                for (TypeId p : v.payload) {
                    good = good && ok(p) && p != id;
                }
            }
            break;
        case TypeKind::Fn:
            for (TypeId e : info.elems) {
                good = good && ok(e);
            }
            good = good && (info.result == kVoid || ok(info.result));
            break;
        case TypeKind::Handle:
        case TypeKind::Param:
            break;
        default:
            good = false;
            break;
        }
        if (info.kind == TypeKind::Nullable && t.kind(info.elem) == TypeKind::Nullable) {
            good = false;
        }
        if (info.array) {
            good = good && info.kind == TypeKind::Tuple && !info.elems.empty() &&
                   info.elems.size() <= kMaxArrayLength &&
                   std::all_of(info.elems.begin(), info.elems.end(), [&](TypeId e) { return e == info.elems[0]; });
        }
        if (info.reprC) {
            good = good && info.kind == TypeKind::Struct;
            auto cValue = [&](TypeId x) { return ok(x) && (t.isScalar(x) || t.info(x).reprC); };
            for (const Field& f : info.fields) {
                bool cArray =
                    ok(f.type) && t.isArray(f.type) && !t.info(f.type).elems.empty() && cValue(t.info(f.type).elems[0]);
                good = good && (cValue(f.type) || cArray);
            }
        }
        if (info.persistPer != 0) {
            good = good && info.kind == TypeKind::State && info.persistPer < t.size() &&
                   t.kind(info.persistPer) == TypeKind::Handle;
        }
        if (!good) {
            return std::unexpected(ValidationError{~0U, ~0U, std::format("malformed type {}", id)});
        }
    }
    return {};
}

} // namespace

std::expected<void, ValidationError> validateTypeGraph(const TypeTable& types) {
    if (auto r = validateTypes(types); !r) {
        return r;
    }
    return TypeGraph(types).run();
}

} // namespace quartz::ir
