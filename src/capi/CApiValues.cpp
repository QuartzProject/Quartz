#include "capi/CApiImpl.hpp"

#include <span>

using quartz::capi::guarded;
using quartz::capi::outcomeOf;
using quartz::capi::toC;
using quartz::capi::trapped;
using quartz::vm::Map;
using quartz::vm::Slot;

namespace {

std::span<const Slot> view(const qz_slot* slots, std::size_t count) {
    return {std::launder(static_cast<const Slot*>(static_cast<const void*>(slots))), count};
}

std::span<Slot> view(qz_slot* slots, std::size_t count) {
    return {std::launder(static_cast<Slot*>(static_cast<void*>(slots))), count};
}

} // namespace

extern "C" {

qz_status qz_sandbox_resume_wide(qz_sandbox* sandbox, uint32_t task, const qz_slot* value, size_t slots,
                                 qz_task_outcome* outcome) {
    return guarded(sandbox, [&] {
        sandbox->error.clear();
        if (quartz::capi::hostValueFailed(sandbox)) {
            return QZ_ERROR;
        }
        auto r = sandbox->sandbox->resume(task, view(value, value != nullptr ? slots : 0));
        if (!r) {
            return trapped(sandbox, r.error());
        }
        if (outcome != nullptr) {
            *outcome = outcomeOf(*r);
        }
        return QZ_OK;
    });
}

uint32_t qz_type_key(const qz_program* program, uint32_t type) {
    const quartz::ir::TypeTable& types = program->program->module->types;
    return type != 0 && type < types.size() && types.kind(type) == quartz::ir::TypeKind::Map ? types.info(type).key : 0;
}

qz_slot qz_sandbox_map(qz_sandbox* sandbox, uint32_t map_type) {
    qz_slot out{};
    out.p = sandbox->sandbox->hostMap(map_type);
    return out;
}

bool qz_map_set(qz_sandbox* sandbox, qz_slot map, const qz_slot* key, const qz_slot* value) {
    auto* m = static_cast<Map*>(map.p);
    return m != nullptr && sandbox->sandbox->hostMapSet(m, view(key, m->keySlots), view(value, m->valueSlots));
}

size_t qz_map_length(qz_slot map) { return map.p != nullptr ? static_cast<const Map*>(map.p)->count : 0; }

bool qz_map_entry(qz_sandbox* sandbox, qz_slot map, size_t index, qz_slot* key_out, qz_slot* value_out) {
    const auto* m = static_cast<const Map*>(map.p);
    return m != nullptr &&
           sandbox->sandbox->hostMapEntry(m, index, view(key_out, m->keySlots), view(value_out, m->valueSlots));
}

bool qz_map_get(qz_sandbox* sandbox, qz_slot map, const qz_slot* key, qz_slot* value_out) {
    const auto* m = static_cast<const Map*>(map.p);
    const Slot* v = m != nullptr ? sandbox->sandbox->hostMapGet(m, view(key, m->keySlots)) : nullptr;
    if (v == nullptr) {
        return false;
    }
    for (std::size_t k = 0; k < m->valueSlots; ++k) {
        value_out[k] = toC(v[k]);
    }
    return true;
}

bool qz_map_set_at(qz_sandbox* sandbox, const qz_slot* map, const qz_slot* key, const qz_slot* value) {
    return qz_map_set(sandbox, *map, key, value);
}

size_t qz_map_length_at(const qz_slot* map) { return qz_map_length(*map); }

bool qz_map_entry_at(qz_sandbox* sandbox, const qz_slot* map, size_t index, qz_slot* key_out, qz_slot* value_out) {
    return qz_map_entry(sandbox, *map, index, key_out, value_out);
}

bool qz_map_get_at(qz_sandbox* sandbox, const qz_slot* map, const qz_slot* key, qz_slot* value_out) {
    return qz_map_get(sandbox, *map, key, value_out);
}

void qz_sandbox_map_into(qz_sandbox* sandbox, uint32_t map_type, qz_slot* out) {
    *out = qz_sandbox_map(sandbox, map_type);
}

} // extern "C"
