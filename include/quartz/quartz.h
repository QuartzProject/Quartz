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
/* A string argument for the next call, valid until the call after it starts. */
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
/* Element of a list or nullable, 0 for other types. */
QZ_API uint32_t qz_type_elem(const qz_program* program, uint32_t type);
/* True for imports the runtime binds itself (the standard library); the host binds the others. */
QZ_API bool qz_program_import_is_native(const qz_program* program, uint32_t import);
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

#ifdef __cplusplus
}
#endif

#endif
