/* The C interface of Quartz. A qz_program may be shared across threads; a qz_sandbox is used by one at a time. */
#ifndef QUARTZ_QUARTZ_H
#define QUARTZ_QUARTZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(QZ_BUILDING_SHARED)
#define QZ_API __declspec(dllexport)
#elif defined(_WIN32) && defined(QZ_USING_SHARED)
#define QZ_API __declspec(dllimport)
#elif defined(__GNUC__)
#define QZ_API __attribute__((visibility("default")))
#else
#define QZ_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change to this header; compare with qz_abi_version() at run time. */
#define QZ_ABI_VERSION 1
/* Raised when functions are added; a host needing newer ones checks qz_api_level() before looking them up. */
#define QZ_API_LEVEL 4

typedef struct qz_program qz_program;
typedef struct qz_sandbox qz_sandbox;
typedef struct qz_call qz_call;

/* One value slot. Scalars use i, u or f; str, lists, maps and states use p (owned by the sandbox). */
typedef union qz_slot {
    int64_t i;
    uint64_t u;
    double f;
    float f32;
    void* p;
} qz_slot;

typedef enum qz_status {
    QZ_OK = 0,
    QZ_ERROR = 1, /* misuse or a bad argument; qz_sandbox_error() or the returned message says why */
    QZ_TRAP = 2   /* the program trapped (fuel, memory, panic, division by zero...); the sandbox stays usable */
} qz_status;

typedef struct qz_source {
    const char* name; /* a name ending in .qzi is a host interface */
    const char* text;
    size_t length;
} qz_source;

typedef struct qz_compile_options {
    bool emit_binary;     /* also produce the portable .qzb module */
    bool keep_lines;      /* keep source lines in the module for trap messages */
    const char* manifest; /* package.toml text, or NULL */
} qz_compile_options;

typedef struct qz_limits {
    size_t memory_bytes;   /* 0 keeps the default (64 MiB) */
    int64_t fuel_per_call; /* 0 keeps the default */
    uint32_t max_depth;    /* 0 keeps the default */
    int tier;              /* 0 default, 1 interpreter only, 2 automatic tiering, 3 native from the first call */
} qz_limits;

typedef enum qz_task_kind {
    QZ_TASK_DONE = 0,
    QZ_TASK_WAIT,
    QZ_TASK_AT_ENTER,
    QZ_TASK_AT_LEAVE,
    QZ_TASK_YIELD
} qz_task_kind;

typedef struct qz_task_outcome {
    qz_task_kind kind;
    uint32_t task;
    qz_slot value; /* DONE: the result; WAIT and AT_*: the awaited handle */
} qz_task_outcome;

/* A host function. Read arguments with qz_call_args, answer with qz_call_set_result; return false to trap. */
typedef bool (*qz_host_fn)(qz_call* call, void* user);

QZ_API uint32_t qz_abi_version(void);
QZ_API uint32_t qz_api_level(void);
/* Frees memory this library returned (messages, binaries, exported state). */
QZ_API void qz_free(void* memory);

/* Compiles sources; NULL on failure. Diagnostics (errors and warnings) go to *diagnostics when not NULL. */
QZ_API qz_program* qz_compile(const qz_source* sources, size_t count, const qz_compile_options* options,
                              char** diagnostics);
/* The .qzb bytes of a program compiled with emit_binary; NULL otherwise. Free with qz_free. */
QZ_API uint8_t* qz_program_binary(const qz_program* program, size_t* length);
/* Checks the hash and, with trusted keys, the Ed25519 signature; decodes and validates a .qzb module. */
QZ_API qz_program* qz_load(const uint8_t* bytes, size_t length, const uint8_t (*trusted_keys)[32], size_t key_count,
                           bool require_signature, char** error);
QZ_API void qz_program_free(qz_program* program);
/* Index of a function by name, or UINT32_MAX. */
QZ_API uint32_t qz_program_function(const qz_program* program, const char* name);

/* A sandbox shares the program; free the program whenever, the sandbox keeps what it needs. */
QZ_API qz_sandbox* qz_sandbox_new(const qz_program* program, const qz_limits* limits);
QZ_API void qz_sandbox_free(qz_sandbox* sandbox);
QZ_API bool qz_sandbox_bind(qz_sandbox* sandbox, const char* import_name, qz_host_fn fn, void* user);
QZ_API qz_status qz_sandbox_start(qz_sandbox* sandbox);
/* A str or list result lives until the next call on this sandbox: copy what you keep. */
QZ_API qz_status qz_sandbox_call(qz_sandbox* sandbox, uint32_t function, const qz_slot* args, size_t count,
                                 qz_slot* result);
QZ_API qz_status qz_sandbox_spawn(qz_sandbox* sandbox, uint32_t function, const qz_slot* args, size_t count,
                                  qz_task_outcome* outcome);
QZ_API qz_status qz_sandbox_resume(qz_sandbox* sandbox, uint32_t task, qz_slot value, qz_task_outcome* outcome);
QZ_API void qz_sandbox_cancel(qz_sandbox* sandbox, uint32_t task);
/* Why the last call failed or trapped, valid until the next call on this sandbox; "" when it did not. */
QZ_API const char* qz_sandbox_error(const qz_sandbox* sandbox);
/* A string argument for the next call, valid until the call after it starts. Text that is not valid UTF-8 gives a
 * 0 slot and makes the next call, spawn or resume fail with QZ_ERROR (Bytes go through qz_sandbox_bytes). */
QZ_API qz_slot qz_sandbox_str(qz_sandbox* sandbox, const char* utf8, size_t length);
QZ_API void qz_sandbox_set_fuel(qz_sandbox* sandbox, int64_t fuel_per_call);
QZ_API int64_t qz_sandbox_fuel_left(const qz_sandbox* sandbox);
QZ_API size_t qz_sandbox_memory_used(const qz_sandbox* sandbox);
/* Every state as JSON (for persistence or reload); NULL on a trap. Free with qz_free. */
QZ_API char* qz_sandbox_export_state(qz_sandbox* sandbox);
QZ_API qz_status qz_sandbox_import_state(qz_sandbox* sandbox, const char* json, size_t length);
/* Binds client's imports `package.fn` to provider's exports, run on its budget; the provider outlives the client. */
QZ_API qz_status qz_sandbox_link(qz_sandbox* client, const char* package, qz_sandbox* provider);

/* The bytes of a str slot (not NUL terminated). */
QZ_API const char* qz_str(qz_slot value, size_t* length);

/* Inside a host function. */
QZ_API const qz_slot* qz_call_args(const qz_call* call);
QZ_API void qz_call_set_result(qz_call* call, qz_slot value);
/* Results wider than one slot (structs, Result) are written here. */
QZ_API qz_slot* qz_call_result_slots(qz_call* call);
/* A str result; text that is not valid UTF-8 traps the call, whatever the host function returns. */
QZ_API qz_slot qz_call_str(qz_call* call, const char* utf8, size_t length);
/* Charges fuel for native work; false when the budget is gone (then return false). */
QZ_API bool qz_call_charge(qz_call* call, int64_t units);
/* Sets the trap message; return false from the host function after it. */
QZ_API void qz_call_fail(qz_call* call, const char* message);
QZ_API qz_sandbox* qz_call_sandbox(qz_call* call);
/* Slots the arguments of this call take (a struct takes one per field). */
QZ_API size_t qz_call_arg_count(const qz_call* call);

/* A host function that gets its arguments and result slot directly, saving two calls through the FFI. */
typedef bool (*qz_host_fn_direct)(qz_call* call, const qz_slot* args, qz_slot* result, void* user);
QZ_API bool qz_sandbox_bind_direct(qz_sandbox* sandbox, const char* import_name, qz_host_fn_direct fn, void* user);

/* Types, so a binding can check arguments before a call: passing a str where an int goes is undefined behaviour. */
typedef enum qz_kind {
    QZ_KIND_VOID = 0,
    QZ_KIND_BOOL,
    QZ_KIND_I8,
    QZ_KIND_I16,
    QZ_KIND_I32,
    QZ_KIND_I64,
    QZ_KIND_U8,
    QZ_KIND_U16,
    QZ_KIND_U32,
    QZ_KIND_U64,
    QZ_KIND_F32,
    QZ_KIND_F64,
    QZ_KIND_CHAR,
    QZ_KIND_STR,
    QZ_KIND_BYTES,
    QZ_KIND_LIST,
    QZ_KIND_MAP,
    QZ_KIND_STRUCT,
    QZ_KIND_ENUM,
    QZ_KIND_NULLABLE,
    QZ_KIND_TUPLE,
    QZ_KIND_HANDLE,
    QZ_KIND_FUNCTION,
    QZ_KIND_OTHER
} qz_kind;

/* Parameters a caller passes to a function (the sandbox supplies `state` parameters itself). */
QZ_API uint32_t qz_program_function_arity(const qz_program* program, uint32_t function);
/* Kind of parameter `index`; `*slots` (when not NULL) gets how many slots it takes. */
QZ_API qz_kind qz_program_function_param(const qz_program* program, uint32_t function, uint32_t index, uint32_t* slots);
QZ_API qz_kind qz_program_function_result(const qz_program* program, uint32_t function, uint32_t* slots);
QZ_API uint32_t qz_program_import_count(const qz_program* program);
/* Name to bind it by (`log`, `Player.health`); valid while the program lives. */
QZ_API const char* qz_program_import_name(const qz_program* program, uint32_t import);
QZ_API uint32_t qz_program_import_arity(const qz_program* program, uint32_t import);
QZ_API qz_kind qz_program_import_param(const qz_program* program, uint32_t import, uint32_t index, uint32_t* slots);
QZ_API qz_kind qz_program_import_result(const qz_program* program, uint32_t import, uint32_t* slots);
/* The .qzi other packages compile against, for a program built with a package.toml; NULL with *error otherwise. */
QZ_API char* qz_program_interface(const qz_program* program, const char* manifest, char** error);

/* Type ids, to build lists for arguments and results and to read nested values; 0 is not a valid type. */
QZ_API uint32_t qz_program_function_param_type(const qz_program* program, uint32_t function, uint32_t index);
QZ_API uint32_t qz_program_function_result_type(const qz_program* program, uint32_t function);
QZ_API uint32_t qz_program_import_param_type(const qz_program* program, uint32_t import, uint32_t index);
QZ_API uint32_t qz_program_import_result_type(const qz_program* program, uint32_t import);
QZ_API qz_kind qz_type_kind(const qz_program* program, uint32_t type, uint32_t* slots);
/* Element of a list or nullable, value of a map, 0 for other types. */
QZ_API uint32_t qz_type_elem(const qz_program* program, uint32_t type);
/* True for imports the runtime binds itself (the standard library); the host binds the others. */
QZ_API bool qz_program_import_is_native(const qz_program* program, uint32_t import);
/* Fields of a struct or elements of a tuple: count, then each one's type id and slot offset (and a struct field's
 * name). */
QZ_API uint32_t qz_type_field_count(const qz_program* program, uint32_t type);
QZ_API uint32_t qz_type_field(const qz_program* program, uint32_t type, uint32_t index, uint32_t* offset);
QZ_API const char* qz_type_field_name(const qz_program* program, uint32_t type, uint32_t index);
/* Enums: slot 0 is the variant index and the payload follows from slot 1. */
QZ_API uint32_t qz_type_variant_count(const qz_program* program, uint32_t type);
QZ_API const char* qz_type_variant_name(const qz_program* program, uint32_t type, uint32_t variant);
QZ_API uint32_t qz_type_variant_payload_count(const qz_program* program, uint32_t type, uint32_t variant);
QZ_API uint32_t qz_type_variant_payload(const qz_program* program, uint32_t type, uint32_t variant, uint32_t index,
                                        uint32_t* offset);
/* A nullable of a reference type is one slot, null when 0; otherwise slot 0 is 1 when present and the value follows. */
QZ_API bool qz_type_nullable_is_pointer(const qz_program* program, uint32_t type);
/* Like qz_sandbox_call, for results wider than one slot: copies `slots` result slots into `result`. */
QZ_API qz_status qz_sandbox_call_wide(qz_sandbox* sandbox, uint32_t function, const qz_slot* args, size_t count,
                                      qz_slot* result, size_t slots);

/* Lists. A list type is an opaque id; qz_sandbox_wait_type gives the one a waiting task expects. */
QZ_API uint32_t qz_sandbox_wait_type(const qz_sandbox* sandbox, uint32_t task);
/* An empty list built like qz_sandbox_str (or in a host function, its result); 0 slot when the type is not a list. */
QZ_API qz_slot qz_sandbox_list(qz_sandbox* sandbox, uint32_t list_type);
/* Appends one element given as its slots; false when the quota is exhausted or the size is wrong. */
QZ_API bool qz_list_push(qz_sandbox* sandbox, qz_slot list, const qz_slot* element, size_t slots);
QZ_API size_t qz_list_length(qz_slot list);
/* Copies element `index` (its slots) into `out`; false out of range. */
QZ_API bool qz_list_get(qz_slot list, size_t index, qz_slot* out, size_t slots);

/* The same functions taking slots by pointer, for FFIs where passing a union by value is awkward (wasm32, some JITs).
 */
QZ_API const char* qz_str_at(const qz_slot* value, size_t* length);
QZ_API size_t qz_list_length_at(const qz_slot* list);
QZ_API bool qz_list_get_at(const qz_slot* list, size_t index, qz_slot* out, size_t slots);
QZ_API bool qz_list_push_at(qz_sandbox* sandbox, const qz_slot* list, const qz_slot* element, size_t slots);
QZ_API qz_status qz_sandbox_resume_at(qz_sandbox* sandbox, uint32_t task, const qz_slot* value,
                                      qz_task_outcome* outcome);
QZ_API void qz_call_str_into(qz_call* call, const char* utf8, size_t length, qz_slot* out);
QZ_API void qz_sandbox_str_into(qz_sandbox* sandbox, const char* utf8, size_t length, qz_slot* out);

/* Waits that produce a struct, tuple, enum or nullable take several slots: resume with all of them. */
QZ_API qz_status qz_sandbox_resume_wide(qz_sandbox* sandbox, uint32_t task, const qz_slot* value, size_t slots,
                                        qz_task_outcome* outcome);

/* Maps iterate in insertion order. Keys and values are given and read as their slots (qz_type_kind counts them). */
QZ_API uint32_t qz_type_key(const qz_program* program, uint32_t type);
/* An empty map built like qz_sandbox_list; 0 slot when the type is not a map. */
QZ_API qz_slot qz_sandbox_map(qz_sandbox* sandbox, uint32_t map_type);
/* Inserts or replaces an entry; false when the quota is exhausted or the map is not one the host may change. */
QZ_API bool qz_map_set(qz_sandbox* sandbox, qz_slot map, const qz_slot* key, const qz_slot* value);
QZ_API size_t qz_map_length(qz_slot map);
/* Copies entry `index` in insertion order; false out of range. Reading the indexes in order is linear overall. */
QZ_API bool qz_map_entry(qz_sandbox* sandbox, qz_slot map, size_t index, qz_slot* key_out, qz_slot* value_out);
/* Copies the value stored under `key`; false when it is absent. */
QZ_API bool qz_map_get(qz_sandbox* sandbox, qz_slot map, const qz_slot* key, qz_slot* value_out);
QZ_API bool qz_map_set_at(qz_sandbox* sandbox, const qz_slot* map, const qz_slot* key, const qz_slot* value);
QZ_API size_t qz_map_length_at(const qz_slot* map);
QZ_API bool qz_map_entry_at(qz_sandbox* sandbox, const qz_slot* map, size_t index, qz_slot* key_out,
                            qz_slot* value_out);
QZ_API bool qz_map_get_at(qz_sandbox* sandbox, const qz_slot* map, const qz_slot* key, qz_slot* value_out);
QZ_API void qz_sandbox_map_into(qz_sandbox* sandbox, uint32_t map_type, qz_slot* out);

/* Trap codes, for hosts that react to the kind of failure. */
typedef enum qz_trap {
    QZ_TRAP_NONE = 0,
    QZ_TRAP_UNREACHABLE,
    QZ_TRAP_DIVIDE_BY_ZERO,
    QZ_TRAP_INDEX_OUT_OF_BOUNDS,
    QZ_TRAP_NULL_UNWRAP,
    QZ_TRAP_OUT_OF_FUEL,
    QZ_TRAP_OUT_OF_MEMORY,
    QZ_TRAP_STACK_OVERFLOW,
    QZ_TRAP_INVALID_HANDLE,
    QZ_TRAP_PANIC,
    QZ_TRAP_HOST_ERROR
} qz_trap;

/* The kind of the last trap, QZ_TRAP_NONE after a call that did not trap. */
QZ_API qz_trap qz_sandbox_trap_code(const qz_sandbox* sandbox);

/* Replaces the program in place, keeping bindings, limits and the state migrated; on failure nothing changes. */
QZ_API qz_status qz_sandbox_reload(qz_sandbox* sandbox, const qz_program* program);
/* `@persist(per = Handle)` states of one handle, as JSON (free with qz_free), and back. */
QZ_API char* qz_sandbox_export_handle(qz_sandbox* sandbox, uint64_t handle);
QZ_API qz_status qz_sandbox_import_handle(qz_sandbox* sandbox, uint64_t handle, const char* json, size_t length);
QZ_API void qz_sandbox_drop_handle(qz_sandbox* sandbox, uint64_t handle);
QZ_API size_t qz_sandbox_live_tasks(const qz_sandbox* sandbox);
QZ_API int64_t qz_sandbox_fuel_per_call(const qz_sandbox* sandbox);

/* API level 4. */

/* Diagnostics one by one. Lines and columns start at 1, columns count bytes; 0 when there is no source place. */
typedef struct qz_diagnostics qz_diagnostics;

typedef enum qz_severity { QZ_SEVERITY_ERROR = 0, QZ_SEVERITY_WARNING, QZ_SEVERITY_NOTE } qz_severity;

typedef enum qz_level { QZ_LEVEL_IGNORE = 0, QZ_LEVEL_WARNING, QZ_LEVEL_ERROR } qz_level;

/* The level of a warning rule (`unused-variable`), or of every rule with "*"; later entries win. */
typedef struct qz_rule_level {
    const char* rule;
    qz_level level;
} qz_rule_level;

/* Like qz_compile, with warning levels; a warning raised to an error fails the compilation. *diagnostics, when not
 * NULL, gets every diagnostic (also on failure); free it with qz_diagnostics_free. */
QZ_API qz_program* qz_compile_with(const qz_source* sources, size_t count, const qz_compile_options* options,
                                   const qz_rule_level* levels, size_t level_count, qz_diagnostics** diagnostics);
QZ_API void qz_diagnostics_free(qz_diagnostics* diagnostics);
QZ_API size_t qz_diagnostics_count(const qz_diagnostics* diagnostics);
/* Everything rendered as qz_compile writes it; valid while the diagnostics live, like every string below. */
QZ_API const char* qz_diagnostics_text(const qz_diagnostics* diagnostics);
QZ_API qz_severity qz_diagnostic_severity(const qz_diagnostics* diagnostics, size_t index);
/* The warning rule id, kept when the warning was raised to an error; "" for other errors and notes. */
QZ_API const char* qz_diagnostic_rule(const qz_diagnostics* diagnostics, size_t index);
QZ_API const char* qz_diagnostic_file(const qz_diagnostics* diagnostics, size_t index);
QZ_API const char* qz_diagnostic_message(const qz_diagnostics* diagnostics, size_t index);
/* Where it starts and ends (the end column is one past the last byte); any pointer may be NULL. */
QZ_API void qz_diagnostic_range(const qz_diagnostics* diagnostics, size_t index, uint32_t* line, uint32_t* column,
                                uint32_t* end_line, uint32_t* end_column);
/* The warning rule ids this library knows. */
QZ_API size_t qz_warning_rule_count(void);
QZ_API const char* qz_warning_rule(size_t index);

typedef enum qz_lookup { QZ_LOOKUP_FOUND = 0, QZ_LOOKUP_NOT_FOUND, QZ_LOOKUP_AMBIGUOUS } qz_lookup;

/* A function by name or by package and name (`org.example.greet`). Exported functions come first, and a name that
 * several share is ambiguous (qz_program_function then gives UINT32_MAX). */
QZ_API qz_lookup qz_program_find_function(const qz_program* program, const char* name, uint32_t* index);
/* An import by its full name (`org.example.host.Player.name`) or, when only one has it, without the package. */
QZ_API qz_lookup qz_program_find_import(const qz_program* program, const char* name, uint32_t* index);

/* What the source wrote: nothing, `private` (not meant for the host), `internal` or `pub`. */
typedef enum qz_visibility {
    QZ_VISIBILITY_DEFAULT = 0,
    QZ_VISIBILITY_PRIVATE,
    QZ_VISIBILITY_INTERNAL,
    QZ_VISIBILITY_PUBLIC
} qz_visibility;

/* The functions a host may call: count, then each one's function index. */
QZ_API uint32_t qz_program_export_count(const qz_program* program);
QZ_API uint32_t qz_program_export(const qz_program* program, uint32_t index);
/* Name with its package (`org.example.greet`, `org.example.Shop.buy`); strings live as long as the program. */
QZ_API const char* qz_program_function_name(const qz_program* program, uint32_t function);
QZ_API const char* qz_program_function_package(const qz_program* program, uint32_t function);
/* Name of parameter `index` as qz_program_function_param counts them; "" for functions that are not exported. */
QZ_API const char* qz_program_function_param_name(const qz_program* program, uint32_t function, uint32_t index);
QZ_API qz_visibility qz_program_function_visibility(const qz_program* program, uint32_t function);
QZ_API bool qz_program_function_is_suspend(const qz_program* program, uint32_t function);
QZ_API bool qz_program_function_is_exported(const qz_program* program, uint32_t function);
QZ_API const char* qz_program_import_package(const qz_program* program, uint32_t import);
/* False for an import no function calls: qz_sandbox_start does not ask for its binding (binding it still works). */
QZ_API bool qz_program_import_is_required(const qz_program* program, uint32_t import);
/* How the source writes a type (`Player`, `[int]`, `Map<str, Pt>`, `Box<int>`), and a nominal type's package. */
QZ_API const char* qz_type_name(const qz_program* program, uint32_t type);
QZ_API const char* qz_type_package(const qz_program* program, uint32_t type);

/* Attributes of a function as written (`@command("heal", perm = "x.heal")`, `@on(Join)`), for hosts that register
 * handlers from them. */
typedef enum qz_attribute_arg_kind {
    QZ_ATTRIBUTE_ARG_INT = 0,
    QZ_ATTRIBUTE_ARG_FLOAT,
    QZ_ATTRIBUTE_ARG_BOOL,
    QZ_ATTRIBUTE_ARG_STR,
    QZ_ATTRIBUTE_ARG_NAME
} qz_attribute_arg_kind;

typedef struct qz_attribute_arg {
    const char* key; /* "" for a positional argument */
    qz_attribute_arg_kind kind;
    int64_t int_value;  /* INT, and BOOL as 0 or 1 */
    double float_value; /* FLOAT */
    const char* text;   /* STR, or the dotted NAME; strings live as long as the program */
} qz_attribute_arg;

QZ_API uint32_t qz_program_function_attribute_count(const qz_program* program, uint32_t function);
QZ_API const char* qz_program_function_attribute_name(const qz_program* program, uint32_t function, uint32_t attribute);
/* `@on(..., where = expr)`: the exported function that tests the handler's first argument; UINT32_MAX otherwise. */
QZ_API uint32_t qz_program_function_attribute_filter(const qz_program* program, uint32_t function, uint32_t attribute);
QZ_API uint32_t qz_program_function_attribute_arg_count(const qz_program* program, uint32_t function,
                                                        uint32_t attribute);
QZ_API bool qz_program_function_attribute_arg(const qz_program* program, uint32_t function, uint32_t attribute,
                                              uint32_t index, qz_attribute_arg* out);

/* Bytes values, any bytes: an argument like qz_sandbox_str, and a result inside a host function. */
QZ_API qz_slot qz_sandbox_bytes(qz_sandbox* sandbox, const uint8_t* data, size_t length);
QZ_API qz_slot qz_call_bytes(qz_call* call, const uint8_t* data, size_t length);
QZ_API void qz_sandbox_bytes_into(qz_sandbox* sandbox, const uint8_t* data, size_t length, qz_slot* out);
QZ_API void qz_call_bytes_into(qz_call* call, const uint8_t* data, size_t length, qz_slot* out);

#ifdef __cplusplus
}
#endif

#endif
