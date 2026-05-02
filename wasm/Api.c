// quartz-capi: every qz_* for JavaScript, plus qzw_* helpers that keep 64-bit values and slots in memory.
#include "quartz/quartz.h"

#include <emscripten/emscripten.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(qz_slot) == 8, "a slot is 8 bytes");
_Static_assert(offsetof(qz_task_outcome, task) == 4 && offsetof(qz_task_outcome, value) == 8, "outcome layout");

/// Compiles `count` sources; `flags` bit 0 emits the .qzb module, bit 1 keeps source lines.
EMSCRIPTEN_KEEPALIVE qz_program* qzw_compile(const char* const* names, const char* const* texts,
                                             const uint32_t* lengths, uint32_t count, int flags, const char* manifest,
                                             char** diagnostics) {
    qz_source sources[64];
    if (count > 64) {
        return NULL;
    }
    for (uint32_t i = 0; i < count; ++i) {
        sources[i].name = names[i];
        sources[i].text = texts[i];
        sources[i].length = lengths[i];
    }
    qz_compile_options options = {(flags & 1) != 0, (flags & 2) != 0, manifest};
    return qz_compile(sources, count, &options, diagnostics);
}

EMSCRIPTEN_KEEPALIVE qz_sandbox* qzw_sandbox_new(const qz_program* program, uint32_t memory, const int64_t* fuel,
                                                 uint32_t depth, int tier) {
    qz_limits limits = {memory, *fuel, depth, tier};
    return qz_sandbox_new(program, &limits);
}

EMSCRIPTEN_KEEPALIVE qz_status qzw_resume(qz_sandbox* sandbox, uint32_t task, const qz_slot* value,
                                          qz_task_outcome* outcome) {
    return qz_sandbox_resume(sandbox, task, *value, outcome);
}

EMSCRIPTEN_KEEPALIVE void qzw_set_fuel(qz_sandbox* sandbox, const int64_t* fuel) {
    qz_sandbox_set_fuel(sandbox, *fuel);
}

/// Fuel left after the last call and fuel per call.
EMSCRIPTEN_KEEPALIVE void qzw_fuel(const qz_sandbox* sandbox, int64_t* out) {
    out[0] = qz_sandbox_fuel_left(sandbox);
    out[1] = qz_sandbox_fuel_per_call(sandbox);
}

EMSCRIPTEN_KEEPALIVE char* qzw_export_handle(qz_sandbox* sandbox, const uint64_t* handle) {
    return qz_sandbox_export_handle(sandbox, *handle);
}

EMSCRIPTEN_KEEPALIVE qz_status qzw_import_handle(qz_sandbox* sandbox, const uint64_t* handle, const char* json,
                                                 size_t length) {
    return qz_sandbox_import_handle(sandbox, *handle, json, length);
}

EMSCRIPTEN_KEEPALIVE void qzw_drop_handle(qz_sandbox* sandbox, const uint64_t* handle) {
    qz_sandbox_drop_handle(sandbox, *handle);
}

EMSCRIPTEN_KEEPALIVE const char* qzw_str(const qz_slot* value, size_t* length) { return qz_str(*value, length); }

EMSCRIPTEN_KEEPALIVE void qzw_sandbox_str(qz_sandbox* sandbox, const char* utf8, size_t length, qz_slot* out) {
    *out = qz_sandbox_str(sandbox, utf8, length);
}

EMSCRIPTEN_KEEPALIVE void qzw_call_str(qz_call* call, const char* utf8, size_t length, qz_slot* out) {
    *out = qz_call_str(call, utf8, length);
}

EMSCRIPTEN_KEEPALIVE bool qzw_call_charge(qz_call* call, const int64_t* units) { return qz_call_charge(call, *units); }

EMSCRIPTEN_KEEPALIVE void qzw_sandbox_list(qz_sandbox* sandbox, uint32_t type, qz_slot* out) {
    *out = qz_sandbox_list(sandbox, type);
}

EMSCRIPTEN_KEEPALIVE bool qzw_list_push(qz_sandbox* sandbox, const qz_slot* list, const qz_slot* element,
                                        size_t slots) {
    return qz_list_push(sandbox, *list, element, slots);
}

EMSCRIPTEN_KEEPALIVE size_t qzw_list_length(const qz_slot* list) { return qz_list_length(*list); }

EMSCRIPTEN_KEEPALIVE bool qzw_list_get(const qz_slot* list, size_t index, qz_slot* out, size_t slots) {
    return qz_list_get(*list, index, out, slots);
}
