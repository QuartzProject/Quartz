#define _POSIX_C_SOURCE 199309L

#include "quartz/quartz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char kHost[] = "fn hostId(x: int) -> int\nfn hostDirect(x: int) -> int\n";
static const char kPlugin[] = "fn id(x: int) -> int {\n    return x\n}\n"
                              "fn spin(n: int) -> int {\n    var s = 0\n    var i = 0\n    while i < n {\n"
                              "        s += i\n        i += 1\n    }\n    return s\n}\n"
                              "fn ups(n: int) -> int {\n    var s = 0\n    var i = 0\n    while i < n {\n"
                              "        s += hostId(i)\n        i += 1\n    }\n    return s\n}\n"
                              "fn upsDirect(n: int) -> int {\n    var s = 0\n    var i = 0\n    while i < n {\n"
                              "        s += hostDirect(i)\n        i += 1\n    }\n    return s\n}\n";

static bool hostId(qz_call* call, void* user) {
    (void)user;
    qz_call_set_result(call, qz_call_args(call)[0]);
    return true;
}

static bool hostDirect(qz_call* call, const qz_slot* args, qz_slot* result, void* user) {
    (void)call;
    (void)user;
    result[0] = args[0];
    return true;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static long long run(qz_sandbox* sb, uint32_t fn, long long arg) {
    qz_slot a = {.i = arg};
    qz_slot r = {0};
    if (qz_sandbox_call(sb, fn, &a, 1, &r) != QZ_OK) {
        fprintf(stderr, "%s\n", qz_sandbox_error(sb));
        exit(1);
    }
    return r.i;
}

int main(int argc, char** argv) {
    long long calls = argc > 1 ? atoll(argv[1]) : 2000000;
    long long inner = argc > 2 ? atoll(argv[2]) : 1000;
    int rounds = argc > 3 ? atoi(argv[3]) : 5;
    qz_source sources[] = {{"host.qzi", kHost, sizeof kHost - 1}, {"bench.qz", kPlugin, sizeof kPlugin - 1}};
    char* diagnostics = NULL;
    qz_program* program = qz_compile(sources, 2, NULL, &diagnostics);
    if (program == NULL) {
        fprintf(stderr, "%s\n", diagnostics);
        return 1;
    }
    qz_free(diagnostics);
    qz_limits limits = {0, 1LL << 40, 0, 1};
    qz_sandbox* sb = qz_sandbox_new(program, &limits);
    qz_sandbox_bind(sb, "hostId", hostId, NULL);
    qz_sandbox_bind_direct(sb, "hostDirect", hostDirect, NULL);
    if (qz_sandbox_start(sb) != QZ_OK) {
        return 1;
    }
    uint32_t id = qz_program_function(program, "id");
    uint32_t spin = qz_program_function(program, "spin");
    uint32_t ups = qz_program_function(program, "ups");
    uint32_t upsDirect = qz_program_function(program, "upsDirect");
    for (int round = 0; round < rounds; ++round) {
        long long check = 0;
        double t0 = now();
        for (long long i = 0; i < calls; ++i) {
            check += run(sb, id, i);
        }
        double t1 = now();
        long long loops = calls / inner * 10;
        for (long long i = 0; i < loops; ++i) {
            check += run(sb, spin, inner);
        }
        double t2 = now();
        for (long long i = 0; i < loops; ++i) {
            check += run(sb, ups, inner);
        }
        double t3 = now();
        for (long long i = 0; i < loops; ++i) {
            check += run(sb, upsDirect, inner);
        }
        double t4 = now();
        if (round == rounds - 1) {
            double perUp = (double)loops * (double)inner;
            printf("c call_ns=%.1f upcall_ns=%.1f direct_upcall_ns=%.1f check=%lld\n", (t1 - t0) / (double)calls,
                   ((t3 - t2) - (t2 - t1)) / perUp, ((t4 - t3) - (t2 - t1)) / perUp, check);
        }
    }
    qz_sandbox_free(sb);
    qz_program_free(program);
    return 0;
}
