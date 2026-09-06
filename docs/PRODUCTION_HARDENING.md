# Production hardening — 2026-09-06

This change fixes the six defect categories reproduced by the independent source
audit and hardens incremental state, worker failure handling and CLI failure
gates. It does not certify complete semantics for every C++ program. The original
failing baseline remains in [Source model audit](SOURCE_MODEL_AUDIT.md).

## Correctness changes

| Defect | Current behavior | Regression evidence |
| --- | --- | --- |
| Unsound complete table target sets | Source patterns produce incomplete candidates with unknown index domains and unproved escape safety. Every recognized stored callback remains a candidate and address roots remain retained. Changed source hashes prevent stale source-pattern analysis. | Straight-line, fill-after-write and conditional-write C++ programs; address-root and completeness assertions. |
| Missing instantiated class members | Compiler-referenced project specialization parents and members retain separate USRs and `Instantiates`/`Specializes` relationships. | Two `Token<Tag>` specializations resolve to distinct constructor/member identities; sample `StrongId` calls resolve. |
| Missing destructor overrides | Compiler override observations include destructors, conversion functions and implicit intermediate destructors. | Direct destructor pair plus sample virtual-base override chain. |
| Lost out-of-line properties | Occurrences persist type and function/variable/template/export properties. Symbols aggregate active contributions instead of retaining the first declaration forever. Definition types take precedence over declaration types. | Definition replacement, TU removal, SQLite reload, property restoration and clean/incremental comparison. |
| Missing automatic destruction | Non-POD automatic local objects and arrays add conservative lifetime dependencies to compiler-exposed destructors, or their record when the destructor is opaque. | Sample `IndexStore` cleanup; pointer/reference/static/thread-local exclusion tests. |
| Missing variable templates | Clang's indexing callbacks provide authoritative variable-template kinds, primary USRs and payload cursors omitted by the normal cursor visitor. Referenced instantiations retain distinct identities and primary links. | Primary `compact`, same-name templates in separate namespaces, typed instantiations and sample `has_qualified_name_v`. |
| Factory/subscript declarations substituted for returned callbacks | Calls through returned function pointers remain separate unresolved dynamic invocations; the inner factory or overloaded subscript is bound independently. | `provide()()` and `table[0]()` regression with exact inner-call and unresolved outer-call counts. |
| Stale types after edits | Same-USR variables receive types from current occurrence evidence; removed observations stop retaining their types. | `int value` to `double value`, compared with clean extraction. |

Destructor edges at declarations describe potential cleanup, not a complete
control-flow model of scope exits, exceptions or lifetime extension. The table
change deliberately withdraws an unsound optimization. It does not introduce a
new proof engine or assert that source-pattern candidates are exhaustive.

The independent audit expectations were corrected where observation cardinality
was different from semantic identity: an out-of-line destructor has declaration
and definition observations, virtual destructor overrides can pass through
implicit bases, and additional concrete template instantiations must not be
mistaken for duplicate source declarations. Sample table checks now require
explicit uncertainty and retention of all four stored callback candidates.

## Operational changes

- Workers join before the returned result vector can be moved. Correctness no
  longer depends on optional named-return-value optimization.
- Throwing or null frontend factories and non-standard extraction exceptions
  produce failed-TU diagnostics instead of terminating the process. Even empty
  exception messages are handled. The complete-model gate preserves the last
  published snapshot after extraction failure.
- Worker counts use full-string numeric parsing. Negative, signed, malformed
  and overflowing values are rejected; effective workers are bounded by pending
  work. Zero retains automatic selection.
- Export refuses to overwrite its input model, including equivalent filesystem
  paths. File write/flush failures produce errors.
- Snapshot validation verifies that persisted boolean symbol properties match
  their active occurrence evidence. Occurrence type references are validated.
- Schema **4**, extraction **15**, canonicalization **8**, flow analysis **3**
  invalidate older snapshots. Use `build` to regenerate them; old files are not
  silently treated as current extraction evidence.
- Independent semantic audits and CLI reliability checks are registered in
  CTest and therefore included in the Windows CI analysis ON/OFF matrix.

## Final verification

| Check | Result |
| --- | --- |
| Release build, analysis ON (`build`) | Passed, including a full clean rebuild |
| Release build, analysis OFF (`build-core`) | Passed |
| CTest, analysis ON | **6/6 suites passed** |
| CTest, analysis OFF | **5/5 suites passed** |
| Conformance seed | **33 C++ fixtures passed** in each configuration |
| Independent defect reproductions | **9/9 programs passed** in each configuration |
| CLI reliability | Input validation, publication preservation, recovery, version rejection and evidence checks passed |
| Local installation smoke test | `cmake --install` succeeded; installed executable loaded libclang and reported a supported toolchain |

The CTest logs are in `build/Testing/Temporary/LastTest.log` and
`build-core/Testing/Temporary/LastTest.log`. Independent per-program JSON audits
are under each build's `semantic-model-verification/audit-regressions` directory.
The CI workflow includes these tests; no remote CI run is claimed here.

Windows intermittently rejected linker writes in this OneDrive workspace with
LNK1104. Retrying the link succeeded. A separate stale object from an in-progress
build was eliminated with a full clean rebuild; both final configurations were
then tested successfully. No source changes were made during the final builds.

The rebuilt sample has **24/24 complete TUs**, **zero diagnostics**, and **31/31
source-audit checks passing**. The snapshot contains 2,913 symbols, 898 logical
symbols, 444 types, 8,976 occurrences and 52,158 relationships across three build
targets and two target dependencies. Both table summaries remain incomplete.
A subsequent build extracts **0** TUs and reuses **24**; observation exports before
and after that build are byte-identical.

The final sample artifacts are:

- `build-core/source-audit/sample-fixed.db`
- `build-core/source-audit/source-audit-fixed.json`
- `build-core/source-audit/final-verification.json`
- `build-core/source-audit/before-reuse.records` and `after-reuse.records`

Reproduce the source audit with:

```powershell
python semantic-model-verification/tools/audit_snapshot.py --model build-core/source-audit/sample-fixed.db --workspace "C:\Users\Animesh Wani\OneDrive\Documents\ChatGPT\sample_prj" --expectations semantic-model-verification/audits/symbolatlas.json --output build-core/source-audit/source-audit-fixed.json
```

The ready-to-run executable is `build/Release/codeinsight.exe`, accompanied by
`libclang.dll`. The local install smoke-test copy is
`build/package-smoke/bin/codeinsight.exe`.

## Release limits

The supported validation environment is Windows x64, MSVC and libclang 22.1.0
from the installed LLVM 22.1.1 distribution. The configured minimum is Clang 20;
this run does not validate every supported compiler version or another platform.

A clean `--require-complete` result is a parse-quality gate for selected TUs.
It does not establish correct build-command discovery, link topology, complete
call resolution or exhaustive language support. The sample audit uses the 24
application Debug/Release commands and authoritative CMake-derived target
membership; the compiler-identification probe is deliberately excluded as build
metadata. No sample source or original compilation database is changed.

General alias/parameter/return flow, member-function pointers, registries and
captured closures remain incomplete. Automatic temporary/reference lifetime
extension, static/TLS cleanup, exact exception/exit paths, and opaque implicit
callable bodies are not exhaustively modeled. Conflicting cross-TU declarations
and ODR violations are not fully diagnosed. Incomplete call sets must not be
used as proof that unlisted targets are unreachable. The optional dead-code
consumer's readiness restrictions remain necessary.

Passing these finite checks supports the implemented boundaries and failure
gates; it does not justify an unrestricted production or universal-C++ claim.
