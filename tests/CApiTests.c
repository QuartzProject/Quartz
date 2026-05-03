#include "quartz/quartz.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                                   \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool greet(qz_call* call, void* user) {
    int* calls = (int*)user;
    size_t len = 0;
    const char* name = qz_str(qz_call_args(call)[0], &len);
    char text[64];
    int n = snprintf(text, sizeof text, "hi %.*s", (int)len, name);
    qz_call_set_result(call, qz_call_str(call, text, (size_t)n));
    *calls += qz_call_arg_count(call) == 1 ? 1 : 100;
    return qz_call_sandbox(call) != NULL;
}

static bool twice(qz_call* call, const qz_slot* args, qz_slot* result, void* user) {
    (void)call;
    (void)user;
    result[0].i = args[0].i * 2;
    return true;
}

static const char kHost[] = "fn greet(name: str) -> str\nfn twice(x: int) -> int\n";
static const char kPlugin[] = "state Counter {\n"
                              "    total: int\n"
                              "}\n"
                              "fn hello(c: Counter, n: int) -> str {\n"
                              "    c.total = c.total + n\n"
                              "    return greet(\"n{c.total}\")\n"
                              "}\n"
                              "fn pair(a: int) -> (int, int) {\n"
                              "    return (twice(a), a + 1)\n"
                              "}\n"
                              "fn divide(a: int, b: int) -> int {\n"
                              "    return a / b\n"
                              "}\n";

int main(void) {
    CHECK(qz_abi_version() == QZ_ABI_VERSION);
    qz_source sources[] = {{"host.qzi", kHost, sizeof kHost - 1}, {"plugin.qz", kPlugin, sizeof kPlugin - 1}};
    qz_compile_options options = {true, true, NULL};
    char* diagnostics = NULL;
    qz_program* program = qz_compile(sources, 2, &options, &diagnostics);
    CHECK(program != NULL);
    if (program == NULL) {
        fprintf(stderr, "%s\n", diagnostics);
        return 1;
    }
    qz_free(diagnostics);

    size_t size = 0;
    uint8_t* binary = qz_program_binary(program, &size);
    CHECK(binary != NULL && size > 0);
    char* error = NULL;
    qz_program* loaded = qz_load(binary, size, NULL, 0, false, &error);
    CHECK(loaded != NULL);
    qz_free(binary);
    qz_program_free(program);

    uint32_t hello = qz_program_function(loaded, "hello");
    uint32_t divide = qz_program_function(loaded, "divide");
    uint32_t pairFn = qz_program_function(loaded, "pair");
    uint32_t pairSlots = 0;
    CHECK(qz_program_function_result(loaded, pairFn, &pairSlots) == QZ_KIND_TUPLE && pairSlots == 2);
    uint32_t pairType = qz_program_function_result_type(loaded, pairFn);
    CHECK(qz_type_kind(loaded, pairType, NULL) == QZ_KIND_TUPLE && qz_type_field_count(loaded, pairType) == 2);
    uint32_t second = 0;
    CHECK(qz_type_kind(loaded, qz_type_field(loaded, pairType, 1, &second), NULL) == QZ_KIND_I64 && second == 1);
    CHECK(qz_api_level() >= 2);
    CHECK(hello != UINT32_MAX && qz_program_function(loaded, "missing") == UINT32_MAX);
    uint32_t slots = 0;
    CHECK(qz_program_function_arity(loaded, hello) == 1);
    CHECK(qz_program_function_param(loaded, hello, 0, &slots) == QZ_KIND_I64 && slots == 1);
    CHECK(qz_program_function_result(loaded, hello, NULL) == QZ_KIND_STR);
    uint32_t greetImport = UINT32_MAX;
    for (uint32_t i = 0; i < qz_program_import_count(loaded); ++i) {
        greetImport = strcmp(qz_program_import_name(loaded, i), "greet") == 0 ? i : greetImport;
    }
    CHECK(greetImport != UINT32_MAX && qz_program_import_param(loaded, greetImport, 0, NULL) == QZ_KIND_STR);
    CHECK(!qz_program_import_is_native(loaded, greetImport));
    qz_limits limits = {0, 0, 0, 1};
    qz_sandbox* sb = qz_sandbox_new(loaded, &limits);
    qz_program_free(loaded);
    int calls = 0;
    CHECK(qz_sandbox_bind(sb, "greet", greet, &calls));
    CHECK(qz_sandbox_bind_direct(sb, "twice", twice, NULL));
    CHECK(qz_sandbox_start(sb) == QZ_OK);

    qz_slot arg = {.i = 5};
    qz_slot result = {0};
    CHECK(qz_sandbox_call(sb, hello, &arg, 1, &result) == QZ_OK);
    CHECK(qz_sandbox_call(sb, hello, &arg, 1, &result) == QZ_OK);
    size_t len = 0;
    const char* text = qz_str_at(&result, &len);
    CHECK(len == 6 && memcmp(text, "hi n10", 6) == 0);
    CHECK(calls == 2);
    char* state = qz_sandbox_export_state(sb);
    CHECK(state != NULL && strstr(state, "10") != NULL);
    qz_free(state);

    qz_slot pair[2] = {{.i = 7}, {.i = 0}};
    CHECK(qz_sandbox_call(sb, divide, pair, 2, &result) == QZ_TRAP);
    CHECK(strstr(qz_sandbox_error(sb), "division") != NULL);
    CHECK(qz_sandbox_trap_code(sb) == QZ_TRAP_DIVIDE_BY_ZERO);
    pair[1].i = 2;
    CHECK(qz_sandbox_call(sb, divide, pair, 2, &result) == QZ_OK && result.i == 3);
    CHECK(qz_sandbox_trap_code(sb) == QZ_TRAP_NONE);
    CHECK(qz_sandbox_call(sb, 9999, NULL, 0, &result) == QZ_ERROR);
    qz_slot wide[2] = {{0}, {0}};
    qz_slot seven = {.i = 7};
    CHECK(qz_sandbox_call_wide(sb, pairFn, &seven, 1, wide, 2) == QZ_OK && wide[0].i == 14 && wide[1].i == 8);
    qz_sandbox_free(sb);

    qz_source broken = {"bad.qz", "fn f() -> int {\n    return \"x\"\n}\n", 32};
    diagnostics = NULL;
    CHECK(qz_compile(&broken, 1, NULL, &diagnostics) == NULL);
    CHECK(diagnostics != NULL && strstr(diagnostics, "error") != NULL);
    qz_free(diagnostics);

    if (failures == 0) {
        printf("C interface: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
