#include "vm/Layout.hpp"

namespace quartz::vm {

using ir::TypeKind;

Layouts::Layouts(const ir::TypeTable& types)
    : types_(types), slots_(types.size(), 0), fieldOffsets_(types.size()), pointerNullable_(types.size(), false), stateSlots_(types.size(), 0),
      state_(types.size(), 0) {
    for (ir::TypeId t = 0; t < types.size(); ++t) {
        compute(t);
    }
}

std::uint32_t Layouts::compute(ir::TypeId t) {
    if (state_[t] == 2) {
        return slots_[t];
    }
    if (state_[t] == 1) {
        return 1; // recursion through a value type is rejected by the validator
    }
    state_[t] = 1;
    const ir::TypeInfo& info = types_.info(t);
    std::uint32_t n = 1;
    switch (info.kind) {
    case TypeKind::Void:
    case TypeKind::Never:
    case TypeKind::Error: n = 0; break;
    case TypeKind::Nullable: {
        TypeKind inner = types_.kind(info.elem);
        bool ptr = types_.isRefKind(info.elem) || inner == TypeKind::Handle;
        pointerNullable_[t] = ptr;
        n = ptr ? 1 : 1 + compute(info.elem);
        break;
    }
    case TypeKind::Struct:
    case TypeKind::State: {
        n = 0;
        for (const ir::Field& f : info.fields) {
            fieldOffsets_[t].push_back(n);
            n += compute(f.type);
        }
        break;
    }
    case TypeKind::Tuple: {
        n = 0;
        for (ir::TypeId e : info.elems) {
            fieldOffsets_[t].push_back(n);
            n += compute(e);
        }
        break;
    }
    case TypeKind::Enum: {
        std::uint32_t payload = 0;
        for (const ir::VariantInfo& v : info.variants) {
            std::uint32_t s = 0;
            for (ir::TypeId p : v.payload) {
                s += compute(p);
            }
            payload = s > payload ? s : payload;
        }
        n = 1 + payload;
        break;
    }
    default: n = 1; break;
    }
    if (info.kind == TypeKind::State) {
        stateSlots_[t] = n;
        n = 1; // a state value is a pointer to its fields
    }
    slots_[t] = n;
    state_[t] = 2;
    return n;
}

} // namespace quartz::vm
