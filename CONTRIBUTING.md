# Contributing

## Building and testing

```bash
cmake --preset debug && cmake --build build/debug && ctest --preset debug
cmake --preset asan  && cmake --build build/asan  && ctest --preset asan
```

Changes to the runtime or the IR also need the native tier: configure a release build with `-DQUARTZ_COMPILER=ON`
(LLVM 23) and run `quartz_native_diff`, which checks that both tiers give the same results. The fuzzers build with the
`fuzz` preset.

## Rules the code follows

- **The sandbox is not traded for speed.** Fuel, quotas and bounds checks stay on; optimisations reduce their cost.
  Anything that reads input (source, modules, data formats) returns errors and never crashes.
- **Determinism.** No fast-math, `-ffp-contract=off` everywhere, no platform math library in results.
- **Performance claims come with numbers:** release build, pinned core, the median of several runs, before and after.
- **Style:** `clang-format` with the repository's configuration, 120 columns; comments only for what the code cannot
  say, one line each.
- **Tests:** few and lasting. Prefer a `.qz` program in `tests/programs` with `// expect:` lines, or an invalid one in
  `tests/diagnostics` with `// error:` lines, over a C++ unit test. Every validator rule has an invalid-IR case.
- **Dependencies:** MIT-compatible licences only, C or C++ without transitive dependencies. LLVM is allowed only in
  the optional native tier.

## Commits

One line, Conventional Commits, in English and in the imperative: `feat(sema): ...`, `fix(vm): ...`,
`perf(compiler): ...`. No body.
