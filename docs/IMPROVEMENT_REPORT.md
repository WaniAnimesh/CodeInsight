# Codebase improvement report — 2026-09-05

> Historical baseline. The subsequent fixes and current verification results
> are recorded in [Production hardening](PRODUCTION_HARDENING.md).

This change addresses verified call-resolution defects and separates the semantic
core from its optional analysis consumer. The attached architecture document was
used as context; this is an incremental improvement, not an implementation of
all C++ analysis mechanisms described there.

## Architecture and baseline evidence

The active path is `main.cpp` -> `IncrementalExecutor::build` -> compilation
commands -> bounded parallel libclang extraction -> immutable `FactBatch` ->
single-threaded canonicalization -> derived calls and entry roots -> semantic
validation -> atomic SQLite publication with read-back equivalence checks.
Incremental execution owns invalidation, checkpoint recovery, and revision
replacement. IDs, symbol variants, observations, and provenance remain owned by
the existing model; libclang cursor types remain inside the frontend.

The initial Windows Release build passed all three CTest suites and all 33
conformance fixtures. A baseline sample run completed all 25 TUs with no warnings,
partial TUs, failed TUs, or diagnostics.

A focused compiled fixture reproduced two defects before editing production code:

1. A local pointer named `value` in an inner scope supplied `unrelated` as a
   target for the outer pointer's call. The old pass joined on caller/name and
   missed the outer pointer's assignments and copies.
2. `value.Base::run()` emitted both a virtual call and an override candidate for
   `Derived::run`, although the explicit qualification suppresses virtual dispatch.

Baseline artifacts are under the ignored `build/call-baseline` directory.
Original versions of initially modified files were retained under `build/original`
for local comparison; the repository had no commits and its source files were
already untracked when work began.

## Changes

- `src/call_resolution.cpp` and its public header provide reusable call resolution
  and target-set summaries, independently of incremental scheduling or analysis.
- `CallableValueFlow` and `InvokesCallable` store compiler-backed storage identity
  and call-site evidence. Local pointer initializers, decay, assignments, copies,
  parentheses, and conditional alternatives feed a revision-scoped work queue.
  Cycles converge; shadowed storage stays separate. Derived edges remain
  conservative and the original unresolved calls remain available.
- Virtual dispatch uses Clang's call-expression classification. Reverse override
  lookup is indexed, and hierarchy closures are cached per declared target.
- `summarize_call_targets` reports complete, incomplete, or external-only results
  with known project targets and supporting relationship IDs. `inspect` displays
  these semantic counts instead of depending on dead-code readiness.
- Dead-code types and API declarations moved to
  `include/codeinsight/analysis/dead_code.hpp`. `src/analysis.cpp` now belongs to
  `codeinsight_analysis`, which depends on `codeinsight_core`. The core does not
  link the analyzer. `CODEINSIGHT_BUILD_ANALYSIS=OFF` excludes the consumer and
  its CLI command. Default builds preserve the existing analysis feature.
  No analysis source was deleted; roots, hierarchy, and bounded-flow facts remain
  generic core capabilities.
- Extraction version increased from 13 to 14 so incremental builds re-extract
  older schema-v3 facts. The SQLite table layout remains v3. Existing analysis
  API clients must include the new header and link the separate library.
- Windows CI now selects LLVM 22.1.1 instead of incompatible LLVM 18.1.8 and
  covers analysis enabled/disabled configurations. Hosted CI was not run here.

The local propagation algorithm sends new targets only along actual copy
constraints. Repeated virtual calls share hierarchy traversal. These are
structural complexity improvements; no measured runtime or memory speedup is
claimed. Working indexes add memory, and the retained bounded-table pass still
has source-pattern matching and graph scans.

## Validation

- Default Windows Release build: successful; 4/4 CTest suites passed.
- Clean semantic-only Windows Release build: successful; 3/3 suites passed.
- All 33 independent conformance fixtures passed in both configurations.
- New regression tests cover shadowing, assignment/copy/conditional/decay flow,
  cycles, unsupported parameter and factory-return flow, qualified and dynamic
  virtual calls, external boundary classification, idempotent resolution,
  SQLite round trips, unchanged-TU reuse, and removal of stale candidates after
  source edits.
- Existing checkpoint-failure/resume and incremental tests passed in the default
  suite. The semantic-only sample reused all 25 TUs on its second run.
- An initial conformance failure exposed duplicate explicit address observations;
  the implementation was corrected and the original assertion retained.
- No compiler warnings were observed in the successful local builds. Changed-file
  whitespace checks passed.

## Reproduce the semantic-only sample run

Run from the repository root:

```powershell
cmake -S . -B build-core -G "Visual Studio 18 2026" -A x64 -DCODEINSIGHT_BUILD_ANALYSIS=OFF
cmake --build build-core --config Release --parallel 4
ctest --test-dir build-core -C Release --output-on-failure

.\build-core\Release\codeinsight.exe build --workspace "C:\Users\Animesh Wani\OneDrive\Documents\ChatGPT\sample_prj" --compile-commands "C:\Users\Animesh Wani\OneDrive\Documents\ChatGPT\sample_prj\build\vs\compile_commands.json" --output .\build-core\sample-model.db --jobs 4 --clean --require-complete
.\build-core\Release\codeinsight.exe validate-model --model .\build-core\sample-model.db
.\build-core\Release\codeinsight.exe inspect --model .\build-core\sample-model.db
.\build-core\Release\codeinsight.exe export --model .\build-core\sample-model.db --view observations --output .\build-core\sample-records-before.txt

# Reuse the completed model:
.\build-core\Release\codeinsight.exe build --workspace "C:\Users\Animesh Wani\OneDrive\Documents\ChatGPT\sample_prj" --compile-commands "C:\Users\Animesh Wani\OneDrive\Documents\ChatGPT\sample_prj\build\vs\compile_commands.json" --output .\build-core\sample-model.db --jobs 4 --require-complete
```

The final model validates and reloads successfully. Default and semantic-only
observation exports are byte-identical, as are the semantic-only exports before
and after the no-op incremental run. No analysis library was produced in the
clean semantic-only build, and its CLI usage contains no `analyze` command.

| Final sample metric | Result |
| --- | ---: |
| Complete translation units | 25 / 25 |
| Warnings / partial / failed TUs | 0 / 0 / 0 |
| Symbols / logical symbols | 2,777 / 859 |
| Types / relationships | 402 / 51,159 |
| Occurrences / diagnostics | 8,622 / 0 |
| Entry roots / bounded-flow summaries | 29 / 2 |
| Complete call target sets | 706 |
| Incomplete call target sets | 1,044 |
| External-only call target sets | 1,456 |
| Re-extracted / reused TUs on second run | 0 / 25 |

TU completeness describes extraction coverage; it does not imply complete
call-target resolution.

## Remaining limits and changed files

Pointer flow is deliberately flow-insensitive and incomplete: assignments after
a call may add conservative candidates. General alias/escape analysis, parameter
and return summaries, registries, arbitrary tables, member-function-pointer
resolution, captured closures, and precise virtual receiver types remain outside
this change. Existing narrow bounded-table summaries are retained, not generalized
into control-flow proofs. Existing template and implicit-callable behavior is
preserved; no new whole-program support is claimed. Non-Windows builds and hosted
CI were not exercised.

New files: `src/call_resolution.cpp`,
`include/codeinsight/semantic/call_resolution.hpp`,
`include/codeinsight/analysis/dead_code.hpp`, `tests/call_resolution_tests.cpp`,
`docs/CALL_RESOLUTION.md`, and this report.

Modified code/build files: `CMakeLists.txt`, `.github/workflows/windows.yml`,
`include/codeinsight/codeinsight.hpp`, `src/libclang_frontend.cpp`,
`src/incremental.cpp`, `src/core.cpp`, `src/main.cpp`, `src/analysis.cpp`, and
`tests/tests.cpp`. Updated documentation: `README.md`, `docs/ARCHITECTURE.md`,
`docs/ANALYSIS.md`, and `docs/RELATIONSHIP_SEMANTICS.md`.
