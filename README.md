<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/QuartzProject/.github/main/.github/assets/quartz-logo-rose.svg">
    <img src="https://raw.githubusercontent.com/QuartzProject/.github/main/.github/assets/quartz-logo-ink-rose.svg" alt="Quartz" width="360">
  </picture>
</p>

<h2 align="center">Quartz</h2>

<p align="center">
  <strong>A statically typed language for embedding, with a real sandbox and native speed.</strong>
</p>

<p align="center">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white">
  <img alt="Platforms" src="https://img.shields.io/badge/platforms-Linux_%7C_Windows_%7C_macOS_%7C_FreeBSD-00599C">
  <img alt="Embedding" src="https://img.shields.io/badge/embeds_in-C%2B%2B_%7C_C_%7C_JVM_%7C_JS-F9A8D4">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
</p>

> ⚠️ **Early development.** The language, the module format and the embedding API still change before 1.0.

## About

Quartz is a general-purpose language for running untrusted code inside an application: game servers, editors, tools,
anything that wants plugins without handing them the process.

- **A real sandbox.** Fuel, memory quota and recursion limit are always on, a program can only call what the host
  binds for it, and every module passes an IR validator before it runs.
- **Fast.** A register interpreter, and an optional LLVM tier that compiles the busy functions with the same semantics
  and the sandbox still enforced.
- **Deterministic.** Wrapping integers, strict IEEE 754 and its own math library: the same bits on every platform and
  in both tiers.
- **Embeds anywhere.** C++ and C, Java and Kotlin on the JVM, Kotlin Multiplatform (Android, Linux, JS) and JavaScript
  through WebAssembly.

```quartz
package org.example.greeter

struct Greeting {
    name: str
    times: int = 1
}

state Stats {
    greeted: Map<str, int>
}

fn greet(stats: Stats, g: Greeting) -> str {
    let total = (stats.greeted.get(g.name) ?? 0) + g.times
    stats.greeted[g.name] = total
    return "Hello, {g.name}! ({total})"
}
```

## Performance

Five plugin event handlers, with the same logic and the same result in every engine:

- **region**: is a broken block inside one of 300 protected areas?
- **econ**: a money transfer between players, with map lookups, a balance formatted with thousands separators and a
  call to the host.
- **chat**: rate limiting, lower-casing and a search for 20 banned words in each message.
- **sim10k**: one physics step over 10 000 moving points.
- **sieve200k**: the prime numbers up to 200 000.

| Workload (ns per call, lower is better) | Budget | region | econ | chat | sim10k | sieve200k |
|---|:---:|---:|---:|---:|---:|---:|
| C, Clang -O2 (reference) | no | 216 | 39 | 553 | 6 378 | 119 892 |
| **Quartz, native tier** | yes | 187 | 177 | 174 | 6 459 | 124 320 |
| Java 25 HotSpot, warmed up | no | 176 | 124 | 126 | 6 431 | 342 127 |
| WebAssembly, wasmtime | no | 313 | 192 | 936 | 8 589 | 242 468 |
| WebAssembly, wasmtime with fuel | yes | 457 | 213 | 1 216 | 25 569 | 341 468 |
| WebAssembly, WAMR fast JIT | no | 416 | 176 | 1 406 | 26 283 | 516 882 |
| LuaJIT 2.1 | no | 1 087 | 788 | 582 | 7 703 | 1 370 122 |
| **Quartz, interpreter** | yes | 2 367 | 278 | 271 | 104 100 | 2 883 662 |
| Luau, native codegen | no | 2 776 | 873 | 1 064 | 106 136 | 6 156 620 |
| WebAssembly, wasm3 interpreter | no | 2 107 | 681 | 11 294 | 79 783 | 3 484 379 |
| Lua 5.5.1 | no | 4 659 | 1 125 | 1 321 | 234 340 | 3 978 659 |
| Lua 5.5.1 with an instruction budget | yes | 4 568 | 1 123 | 1 346 | 240 637 | 4 083 641 |
| Luau, interpreter | yes | 7 011 | 882 | 1 160 | 310 315 | 6 007 734 |
| Wren | no | 16 856 | 2 145 | 1 835 | 1 007 728 | 21 434 541 |
| AngelScript 2.39 | no | 20 893 | 2 038 | 3 617 | 763 242 | 9 754 115 |

**Budget** means the engine can stop code that runs too long; Quartz also enforces its memory quota and recursion limit
in both tiers. Release builds on a Ryzen 7 5800X, one pinned core, median of nine runs.

## Embedding

```cpp
quartz::SourceInput sources[] = {{"host.qzi", "fn shout(text: str) -> str\n"}, {"plugin.qz", pluginSource}};
quartz::CompileOutput out = quartz::compile(sources);
quartz::vm::Sandbox sandbox(out.program, {.memoryBytes = 16 << 20, .fuelPerCall = 1'000'000});
quartz::bind<&shout>(sandbox, "shout");
sandbox.start();
auto result = sandbox.call(out.program->findFunction("run"), {});
```

Other languages use the C interface (`include/quartz/quartz.h` and `libquartz`), the Java and Kotlin bindings in
[`bindings/jvm`](bindings/jvm) or the WebAssembly build in [`wasm/`](wasm).

## Getting started

Everything is in [the Quartz book](https://quartzlang.org/docs/getting-started.html), and the
[playground](https://quartzlang.org/playground/) runs Quartz in the browser. The `quartz` tool checks, runs, tests,
formats and debugs single packages; [Qiln](https://github.com/QuartzProject/Qiln) builds projects with dependencies.

## Download

Every commit to `main` publishes a release, numbered `0.<n>` until 1.0, on the
[releases page](https://github.com/QuartzProject/Quartz/releases). Each one has `quartz`, `quartz-lsp`, `libquartz`
with its headers and the native tier for Linux, Windows, macOS and FreeBSD, plus the WebAssembly build and the JVM
bindings, with a `SHA256SUMS` file.

## Building

```bash
cmake --preset release
cmake --build build/release
```

Needs Clang with C++23, CMake 3.28+ and Ninja. The LLVM native tier and the JVM bindings are built when LLVM 23 and a
JDK are found; `cmake --preset wasm` builds for WebAssembly with Emscripten.

## Contributing

Read the [contribution guide](CONTRIBUTING.md). Report vulnerabilities privately through the
[security policy](SECURITY.md).

## License

[MIT](LICENSE).
