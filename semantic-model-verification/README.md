# CodeInsight Semantic Model Conformance

This sub-project independently verifies the observable CodeInsight Canonical Semantic Model. Each fixture is a complete miniature C or C++ project with an explicit compilation configuration and declarative semantic expectations.

The runner uses only supported public interfaces: it materializes the fixture, generates `compile_commands.json`, invokes `IncrementalExecutor`, reloads the published SQLite snapshot through `SQLiteSnapshotStore`, runs `SemanticModelValidator`, normalizes unstable storage details, and evaluates semantic assertions. It does not reproduce compiler extraction or canonicalization logic.

## Build and run

From the repository root:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel
.\build\semantic-model-verification\Release\semantic-test.exe run --all --fixtures .\semantic-model-verification\fixtures
```

Selections and reports:

```powershell
semantic-test run --category templates --json results.json --junit results.xml
semantic-test run --fixture declaration_definition
semantic-test inspect function_overloads
semantic-test update-golden function_overloads
semantic-test list
```

A failed fixture preserves its input project, compilation database, SQLite model, normalized model, validator result, semantic diff, and reproducibility metadata. Use `--artifacts PATH` to select the retention location.

## Implemented foundation

The project implements phases V0 through V7 of the greenfield plan: contract documentation, discovery and execution, public model loading, deterministic normalization, semantic queries and assertions, mandatory global validation, the 30-fixture seed suite, golden snapshots, semantic diffs, JSON/JUnit results, and failure artifacts. Later lifecycle, mutation, corpus, property, fuzz, and performance phases have explicit extension points but are not misrepresented as complete.

See `docs/IMPLEMENTATION_STATUS.md` for the phase-by-phase boundary.
