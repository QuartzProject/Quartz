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

static const char kMapHost[] = "struct Pt {\n    x: int\n    y: int\n}\n@handle\n@awaitable(Pt)\nstruct AskPt {}\n"
                               "@handle\n@awaitable(Map::<str, int>)\nstruct AskMap {}\n"
                               "fn askPt() -> AskPt\nfn askMap() -> AskMap\n";
static const char kMapPlugin[] = "fn sum(m: Map<str, int>) -> int {\n"
                                 "    var s = 0\n"
                                 "    for k, v in m {\n"
                                 "        s += v * k.len()\n"
                                 "    }\n"
                                 "    return s\n"
                                 "}\n"
                                 "fn table() -> Map<str, int> {\n"
                                 "    var m: Map<str, int> = Map()\n"
                                 "    for i in 0..6 {\n"
                                 "        m[\"k{i}\"] = i\n"
                                 "    }\n"
                                 "    m.remove(\"k1\")\n"
                                 "    m.remove(\"k4\")\n"
                                 "    return m\n"
                                 "}\n"
                                 "suspend fn task() -> int {\n"
                                 "    let p = wait askPt()\n"
                                 "    let m = wait askMap()\n"
                                 "    return p.x * 100 + p.y + sum(m)\n"
                                 "}\n";

static bool handle(qz_call* call, const qz_slot* args, qz_slot* result, void* user) {
    (void)call;
    (void)args;
    (void)user;
    result[0].u = 1;
    return true;
}

static void maps(void) {
    qz_source sources[] = {{"host.qzi", kMapHost, sizeof kMapHost - 1},
                           {"plugin.qz", kMapPlugin, sizeof kMapPlugin - 1}};
    char* diagnostics = NULL;
    qz_program* program = qz_compile(sources, 2, NULL, &diagnostics);
    CHECK(program != NULL);
    if (program == NULL) {
        fprintf(stderr, "%s\n", diagnostics);
    }
    qz_free(diagnostics);
    if (program == NULL) {
        return;
    }
    CHECK(qz_api_level() >= 3);
    uint32_t sum = qz_program_function(program, "sum");
    uint32_t mapType = qz_program_function_param_type(program, sum, 0);
    CHECK(qz_type_kind(program, mapType, NULL) == QZ_KIND_MAP);
    CHECK(qz_type_kind(program, qz_type_key(program, mapType), NULL) == QZ_KIND_STR);
    CHECK(qz_type_kind(program, qz_type_elem(program, mapType), NULL) == QZ_KIND_I64);
    qz_limits limits = {0, 0, 0, 0};
    qz_sandbox* sb = qz_sandbox_new(program, &limits);
    CHECK(qz_sandbox_bind_direct(sb, "askPt", handle, NULL) && qz_sandbox_bind_direct(sb, "askMap", handle, NULL));
    CHECK(qz_sandbox_start(sb) == QZ_OK);

    qz_slot table = {0};
    CHECK(qz_sandbox_call(sb, qz_program_function(program, "table"), NULL, 0, &table) == QZ_OK);
    CHECK(qz_map_length(table) == 4);
    const char* order[] = {"k0", "k2", "k3", "k5"};
    for (size_t i = 0; i < 4; ++i) {
        qz_slot key = {0};
        qz_slot value = {0};
        size_t len = 0;
        CHECK(qz_map_entry_at(sb, &table, i, &key, &value));
        CHECK(memcmp(qz_str(key, &len), order[i], 2) == 0 && len == 2 && value.i == (int64_t)(order[i][1] - '0'));
    }
    qz_slot key = {0};
    qz_slot value = {0};
    CHECK(!qz_map_entry(sb, table, 4, &key, &value));
    qz_sandbox_str_into(sb, "k3", 2, &key);
    CHECK(qz_map_get(sb, table, &key, &value) && value.i == 3);
    qz_sandbox_str_into(sb, "k1", 2, &key);
    CHECK(!qz_map_get_at(sb, &table, &key, &value));

    qz_slot built = qz_sandbox_map(sb, mapType);
    const char* words[] = {"ab", "c", "ab"};
    for (int i = 0; i < 3; ++i) {
        qz_slot k = qz_sandbox_str(sb, words[i], strlen(words[i]));
        qz_slot v = {.i = i + 2};
        CHECK(qz_map_set(sb, built, &k, &v));
    }
    CHECK(qz_map_length_at(&built) == 2);
    qz_slot total = {0};
    CHECK(qz_sandbox_call(sb, sum, &built, 1, &total) == QZ_OK && total.i == 4 * 2 + 3);

    qz_task_outcome t;
    CHECK(qz_sandbox_spawn(sb, qz_program_function(program, "task"), NULL, 0, &t) == QZ_OK && t.kind == QZ_TASK_WAIT);
    uint32_t pt = qz_sandbox_wait_type(sb, t.task);
    CHECK(qz_type_kind(program, pt, NULL) == QZ_KIND_STRUCT);
    qz_slot point[2] = {{.i = 4}, {.i = 2}};
    CHECK(qz_sandbox_resume_wide(sb, t.task, point, 1, &t) != QZ_OK);
    CHECK(qz_sandbox_resume_wide(sb, t.task, point, 2, &t) == QZ_OK && t.kind == QZ_TASK_WAIT);
    qz_slot m = {0};
    qz_sandbox_map_into(sb, qz_sandbox_wait_type(sb, t.task), &m);
    qz_slot k = qz_sandbox_str(sb, "xyz", 3);
    qz_slot one = {.i = 1};
    CHECK(qz_map_set_at(sb, &m, &k, &one));
    CHECK(qz_sandbox_resume_wide(sb, t.task, &m, 1, &t) == QZ_OK && t.kind == QZ_TASK_DONE && t.value.i == 405);
    qz_sandbox_free(sb);
    qz_program_free(program);
}

static void diagnosticTests(void) {
    static const char kLint[] = "fn f(x: int) -> int {\n    let y = 1\n    return x\n}\n";
    qz_source source = {"lint.qz", kLint, sizeof kLint - 1};
    qz_diagnostics* d = NULL;
    qz_program* program = qz_compile_with(&source, 1, NULL, NULL, 0, &d);
    CHECK(program != NULL && qz_diagnostics_count(d) == 1);
    uint32_t line = 0;
    uint32_t column = 0;
    uint32_t endColumn = 0;
    qz_diagnostic_range(d, 0, &line, &column, NULL, &endColumn);
    CHECK(qz_diagnostic_severity(d, 0) == QZ_SEVERITY_WARNING &&
          strcmp(qz_diagnostic_rule(d, 0), "unused-variable") == 0);
    CHECK(strcmp(qz_diagnostic_file(d, 0), "lint.qz") == 0 && line == 2 && column == 9 && endColumn == 10);
    CHECK(strstr(qz_diagnostics_text(d), "[unused-variable]") != NULL);
    qz_diagnostics_free(d);
    qz_program_free(program);

    qz_rule_level deny[] = {{"*", QZ_LEVEL_ERROR}, {"unused-parameter", QZ_LEVEL_IGNORE}};
    CHECK(qz_compile_with(&source, 1, NULL, deny, 2, &d) == NULL);
    CHECK(qz_diagnostics_count(d) == 1 && qz_diagnostic_severity(d, 0) == QZ_SEVERITY_ERROR);
    CHECK(strcmp(qz_diagnostic_rule(d, 0), "unused-variable") == 0);
    qz_diagnostics_free(d);
    qz_rule_level quiet[] = {{"unused-variable", QZ_LEVEL_IGNORE}};
    program = qz_compile_with(&source, 1, NULL, quiet, 1, &d);
    CHECK(program != NULL && qz_diagnostics_count(d) == 0);
    qz_diagnostics_free(d);
    qz_program_free(program);
    CHECK(qz_warning_rule_count() > 10 && strcmp(qz_warning_rule(0), "unused-variable") == 0);
}

static bool playerName(qz_call* call, const qz_slot* args, qz_slot* result, void* user) {
    (void)args;
    (void)user;
    *result = qz_call_str(call, "Ann", 3);
    return true;
}

static void packages(void) {
    static const char kHostIface[] = "package org.example.host\n@handle\nstruct Player {}\n"
                                     "impl Player {\n    fn name(self) -> str\n}\nfn unused(n: int)\n";
    static const char kA[] = "package org.example.a\nimport org.example.host.Player\n"
                             "pub struct Pt {\n    x: int\n}\n"
                             "pub fn greet(p: Player, times: int) -> str {\n    return p.name().repeat(times)\n}\n"
                             "suspend fn later() {}\n";
    static const char kB[] = "package org.example.b\nfn greet() -> int {\n    return 2\n}\n";
    qz_source sources[] = {
        {"host.qzi", kHostIface, sizeof kHostIface - 1}, {"a.qz", kA, sizeof kA - 1}, {"b.qz", kB, sizeof kB - 1}};
    qz_compile_options options = {true, false, NULL};
    char* diagnostics = NULL;
    qz_program* compiled = qz_compile(sources, 3, &options, &diagnostics);
    CHECK(compiled != NULL);
    if (compiled == NULL) {
        fprintf(stderr, "%s\n", diagnostics);
        qz_free(diagnostics);
        return;
    }
    qz_free(diagnostics);
    size_t size = 0;
    uint8_t* binary = qz_program_binary(compiled, &size);
    qz_program* program = qz_load(binary, size, NULL, 0, false, NULL);
    qz_free(binary);
    qz_program_free(compiled);

    uint32_t greet = UINT32_MAX;
    CHECK(qz_program_function(program, "greet") == UINT32_MAX);
    CHECK(qz_program_find_function(program, "greet", NULL) == QZ_LOOKUP_AMBIGUOUS);
    CHECK(qz_program_find_function(program, "org.example.a.greet", &greet) == QZ_LOOKUP_FOUND);
    CHECK(strcmp(qz_program_function_name(program, greet), "org.example.a.greet") == 0);
    CHECK(qz_program_function_visibility(program, greet) == QZ_VISIBILITY_PUBLIC);
    CHECK(strcmp(qz_program_function_param_name(program, greet, 1), "times") == 0);
    uint32_t player = qz_program_function_param_type(program, greet, 0);
    CHECK(strcmp(qz_type_name(program, player), "Player") == 0);
    CHECK(strcmp(qz_type_package(program, player), "org.example.host") == 0);
    uint32_t later = qz_program_function(program, "later");
    CHECK(qz_program_function_is_suspend(program, later) && qz_program_function_is_exported(program, later));
    CHECK(qz_program_export_count(program) == 3);

    uint32_t name = UINT32_MAX;
    uint32_t unused = UINT32_MAX;
    CHECK(qz_program_find_import(program, "Player.name", &name) == QZ_LOOKUP_FOUND);
    CHECK(strcmp(qz_program_import_name(program, name), "org.example.host.Player.name") == 0);
    CHECK(qz_program_find_import(program, "unused", &unused) == QZ_LOOKUP_FOUND);
    CHECK(qz_program_import_is_required(program, name) && !qz_program_import_is_required(program, unused));
    qz_limits limits = {0, 0, 0, 0};
    qz_sandbox* sb = qz_sandbox_new(program, &limits);
    CHECK(qz_sandbox_bind_direct(sb, "Player.name", playerName, NULL));
    CHECK(qz_sandbox_start(sb) == QZ_OK);
    qz_slot args[2] = {{.u = 1}, {.i = 2}};
    qz_slot result = {0};
    size_t len = 0;
    CHECK(qz_sandbox_call(sb, greet, args, 2, &result) == QZ_OK);
    CHECK(memcmp(qz_str(result, &len), "AnnAnn", 6) == 0 && len == 6);
    qz_sandbox_free(sb);
    qz_program_free(program);
}

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

    maps();
    diagnosticTests();
    packages();
    if (failures == 0) {
        printf("C interface: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
