# Source-to-model correctness audit — 2026-09-05

> Historical baseline. The subsequent fixes and current verification results
> are recorded in [Production hardening](PRODUCTION_HARDENING.md).

**Verdict: build and execution pass; semantic correctness does not fully pass.**
The snapshot is structurally valid, reproducible, and correctly represents many
source relationships. It has confirmed semantic omissions, and broader C++
counterexamples expose an unsound complete-target claim. This audit does not
certify arbitrary C++ correctness.

## What was built and run

`build-core/Release/codeinsight.exe` was built successfully with MSVC Release,
using the semantic-only configuration (`CODEINSIGHT_BUILD_ANALYSIS=OFF`). No
production source was changed during this verification task. No sample-specific
engine behavior was added. The work added independent audit tools, expectations,
and reproducible counterexamples.

The original compilation database contains 25 commands: Debug and Release for
12 application source files, plus one CMake compiler-identification probe. The
unmodified database was first analyzed successfully into
`build-core/source-audit/sample-raw.db` (25 complete TUs).

The sample CMake source lists and dependencies were then inspected. An audit-side
copy of the database retains the 24 application commands verbatim, including their
original working directories, flags, definitions, and include paths. Only the
compiler probe, which is not a member of any application target, was excluded.
The sample repository and its compilation database were not modified. Generated
project headers remain modeled; there is no blanket generated-file exclusion.

The accompanying target manifest describes `AtlasCore` (static library),
`AtlasCli` (executable), and `AtlasTests` (test), with both executables depending
on `AtlasCore`. The resulting primary audited model is:

`build-core/source-audit/sample-model.db`

## Validation results

| Check | Result |
| --- | --- |
| Build `codeinsight.exe` | Passed |
| Raw compilation database run | 25/25 complete TUs |
| Audited application input run | 24/24 complete TUs |
| Warnings / partial / failed TUs | 0 / 0 / 0 |
| Diagnostics | 0 |
| Structural model validation / SQLite integrity / foreign keys | Passed |
| Every modeled file-version hash compared with current source bytes | Passed |
| All occurrence and relationship byte/line coordinates checked | Passed |
| Existing CTest suites | 3/3 passed; all 33 conformance fixtures passed |
| Independent source audit | **26 passed, 5 failed** |
| Isolated C++ reproduction programs | **1 passed, 5 failed** |
| Unchanged incremental rerun | 0 extracted, 24 reused |
| Before/after observation exports | Byte-identical |

Final model counts: **2,765 symbols**, **847 logical symbols**, **395 types**,
**8,610 occurrences**, **51,078 relationships**, **3 build targets**, and
**2 target dependencies**. The model has 706 complete, 1,044 incomplete, and
1,456 external-only compiler call target sets. Extraction completeness is not
call-resolution completeness.

## Source-backed checks that passed

The expectation file was authored from source, not generated from model output.
Each source-specific assertion retains the relevant file and source text anchor
in the JSON result. In addition to input and range validation, checks cover:

- The two application process-entry identities remain distinct.
- Shared-header `SourceRange` symbols unify within each configuration.
- `IndexStore` forward declarations and definitions share a logical identity.
- The three `Resolver::resolve` overloads and both diagnostic `append` overloads
  remain distinct.
- Static member declarations and out-of-line definitions unify.
- All six virtual-diamond inheritance relations retain their virtual flags.
- Both `describe` methods override `GraphEntity::describe`.
- The app's `Analyzer::ingest` call binds to its compiler-resolved declaration.
- The Release-only `shrink_to_fit` branch stays configuration-specific.
- The generated version header and macro are retained.
- `TypeDescriptor` primary, partial, explicit, and instantiated identities stay
  distinct.
- This sample's route domain is exactly `{0,1}` and its selected functions are
  `score_identity` and `score_extent` in both configurations.
- Runtime strings such as `atlas::engine::convert` and `bulk::partition_*` do not
  become C++ declarations. Those are data in the fixture, not compiler symbols.

## Confirmed defects and gaps

### 1. P1: bounded-table summaries can falsely claim completeness

`src/call_resolution.cpp:157` recognizes declarations, fills, and slot writes
with independent regular-expression searches. It does not establish execution
order or whether a write executes. `src/call_resolution.cpp:250` nevertheless
sets `complete=true` and can discharge address roots.

Two unrelated, deterministic programs reproduce this:

```cpp
entries[0] = &beta;
entries.fill(&alpha);             // slot 0 ends as alpha
```

```cpp
entries.fill(&alpha);
if (false) entries[0] = &beta;   // slot 0 remains alpha
```

Both use a known index of zero. MSVC-compiled executions return **11**, the return
value of `alpha`; the model reports a **complete** set containing only `beta`,
which returns **22**. A straight-line fill-then-override control returns 22 and
correctly models `beta`. Runtime and model results are saved in
`build-core/source-audit/native-results.json`.

This is a soundness defect, not just an imprecise superset or missing coverage.
Any general fix needs compiler-backed execution/value-flow evidence, or must
retain incomplete sets and address roots when that evidence is unavailable.
A name-based exception for this sample would not fix it.

### 2. P2: compiler-resolved class-template member targets are missing

`app/main.cpp:14` calls `report.revision.value()`. Twenty `StrongId<...>::value`
call observations across the selected input lack a canonical target even though
Clang supplies target USRs. The isolated `Token<Left>` / `Token<Right>` fixture
also loses both constructor and both `value()` call targets.

`src/libclang_frontend.cpp:454` returns from `ensure_implicit_callable` when the
specialized parent class has not already been emitted. A general fix must retain
the compiler-identified parent specialization and member identities without
collapsing them into a primary-template method or guessing by name.

### 3. P2: virtual destructor override relationships are missing

The sample declares `~IndexedSymbol() override` in
`include/atlas/virtual_model.hpp`, but there is no `Overrides` edge to
`GraphEntity::~GraphEntity`. `src/libclang_frontend.cpp:545` retrieves overridden
cursors only for `CXCursor_CXXMethod`, excluding destructor cursors. An unrelated
`Parent`/`Child` fixture reproduces the missing relationship.

### 4. P2: out-of-line defaulted properties are lost

The definitions of `IndexStore::~IndexStore`, `GraphEntity::~GraphEntity`, and
`IndexedSymbol::~IndexedSymbol` use `= default`. All six definition observations
across Debug/Release refer to symbols lacking the defaulted flag.

`src/model.cpp:149` initializes properties from the first installed observation;
`src/model.cpp:180` updates exports and parent relationships but does not aggregate
the later definition's callable properties. A general correction also needs to
handle revision replacement so removed properties do not remain stale.

### 5. P2 coverage gap: automatic object destruction is not represented

The app's automatic `IndexStore store` is constructed but has no `Destroys` edge
at scope exit. There are **zero `Destroys` relationships in the sample model**.
The standalone local `Child value` fixture confirms this limitation. Explicit
call/delete handling does not cover implicit cleanup, early returns, unwinding,
or base/member destruction. A compiler cleanup/CFG-backed representation is
needed before claiming complete lifecycle coverage.

### 6. P2 coverage gap: variable-template primary declarations are absent

`atlas::has_qualified_name_v` is declared in `include/atlas/templates.hpp`, but
its variable-template symbol is absent. A standalone
`template<class T> inline constexpr bool compact = sizeof(T) < 8` reproduction
also lacks the expected primary variable-template representation. The model enum
contains `VariableTemplate`, but the active cursor-kind mapping does not produce
that semantic declaration for these inputs.

## Reproduce the audit

From the repository root:

```powershell
cmake --build build-core --config Release --parallel 4

.\build-core\Release\codeinsight.exe build --workspace "C:\Users\Animesh Wani\OneDrive\Documents\ChatGPT\sample_prj" --compile-commands .\build-core\source-audit\compile_commands.json --target-manifest .\build-core\source-audit\targets.ci --output .\build-core\source-audit\sample-model.db --jobs 4 --clean --require-complete
.\build-core\Release\codeinsight.exe validate-model --model .\build-core\source-audit\sample-model.db

python semantic-model-verification/tools/audit_snapshot.py --model build-core/source-audit/sample-model.db --workspace "C:\Users\Animesh Wani\OneDrive\Documents\ChatGPT\sample_prj" --expectations semantic-model-verification/audits/symbolatlas.json --output build-core/source-audit/source-audit.json
python semantic-model-verification/tools/reproduce_audit.py --codeinsight build-core/Release/codeinsight.exe --cases semantic-model-verification/audits/cpp-reproductions.json --output-dir build-core/source-audit/reproductions

ctest --test-dir build-core -C Release --output-on-failure
```

The independent audit commands intentionally return exit 1 for the confirmed
failures. They do not suppress failures or rewrite expected values. The normal
conformance suite remains green because it does not cover these cases.

The compiled table execution check used:

```powershell
cmake -S build-core/source-audit/reproductions -B build-core/source-audit/native -G "Visual Studio 18 2026" -A x64
cmake --build build-core/source-audit/native --config Release --parallel 3
python build-core/source-audit/run_native.py
```

## Artifacts and limits

Reusable tools and test data are under `semantic-model-verification/tools` and
`semantic-model-verification/audits`. All generated models, prepared compilation
inputs, excluded-input evidence, logs, JSON results, native counterexamples,
exports, and artifact hashes are under `build-core/source-audit` (ignored by Git).
The audit-side preparation script there records how the CMake membership was
translated into inputs. The raw original-database model is retained alongside
the model with explicit target membership.

This is a focused source audit, not an exhaustive proof of the C++ language or
all relationships in the sample. General alias/escape analysis, framework
callbacks, member-function pointers, dependent instantiation bodies, concrete
receiver narrowing, cross-target linking semantics, and non-Windows behavior
remain beyond the demonstrated guarantees. Existing passing tests establish
only their tested contracts. The confirmed failures above prevent a claim that
the current semantic model is universally correct.
