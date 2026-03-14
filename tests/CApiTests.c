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
    ++*calls;
    return qz_call_sandbox(call) != NULL;
}

static const char kHost[] = "fn greet(name: str) -> str\n";
static const char kPlugin[] = "state Counter {\n"
                              "    total: int\n"
                              "}\n"
                              "fn hello(c: Counter, n: int) -> str {\n"
                              "    c.total = c.total + n\n"
                              "    return greet(\"n{c.total}\")\n"
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
    CHECK(hello != UINT32_MAX && qz_program_function(loaded, "missing") == UINT32_MAX);
    qz_limits limits = {0, 0, 0, 1};
    qz_sandbox* sb = qz_sandbox_new(loaded, &limits);
    qz_program_free(loaded);
    int calls = 0;
    CHECK(qz_sandbox_bind(sb, "greet", greet, &calls));
    CHECK(qz_sandbox_start(sb) == QZ_OK);

    qz_slot arg = {.i = 5};
    qz_slot result = {0};
    CHECK(qz_sandbox_call(sb, hello, &arg, 1, &result) == QZ_OK);
    CHECK(qz_sandbox_call(sb, hello, &arg, 1, &result) == QZ_OK);
    size_t len = 0;
    const char* text = qz_str(result, &len);
    CHECK(len == 6 && memcmp(text, "hi n10", 6) == 0);
    CHECK(calls == 2);
    char* state = qz_sandbox_export_state(sb);
    CHECK(state != NULL && strstr(state, "10") != NULL);
    qz_free(state);

    qz_slot pair[2] = {{.i = 7}, {.i = 0}};
    CHECK(qz_sandbox_call(sb, divide, pair, 2, &result) == QZ_TRAP);
    CHECK(strstr(qz_sandbox_error(sb), "division") != NULL);
    pair[1].i = 2;
    CHECK(qz_sandbox_call(sb, divide, pair, 2, &result) == QZ_OK && result.i == 3);
    CHECK(qz_sandbox_call(sb, 9999, NULL, 0, &result) == QZ_ERROR);
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
