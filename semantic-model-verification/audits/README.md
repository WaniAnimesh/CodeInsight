# Independent source audits

These audits compare snapshots with separately authored source expectations.
They complement the normal conformance suite; a successful parse and SQLite
validation do not establish semantic correctness. No expectation is inferred
from the model under test and no failing check is converted to a golden file.

`symbolatlas.json` contains sample-specific assertions as test data. Engine code
contains no SymbolAtlas names or special cases. Run the generic checker with a
model built from the 24 application Debug/Release commands and an authoritative
three-target manifest:

```powershell
python semantic-model-verification/tools/audit_snapshot.py --model build-core/source-audit/sample-model.db --workspace "C:\Users\Animesh Wani\OneDrive\Documents\ChatGPT\sample_prj" --expectations semantic-model-verification/audits/symbolatlas.json --output build-core/source-audit/source-audit.json
```

The paths above identify the external fixture on the audited machine. The
checker itself accepts any workspace and JSON expectation file. SQL checks open
SQLite read-only, and source anchors and file hashes ensure evidence matches the
current workspace bytes.

`cpp-reproductions.json` contains nine isolated C++ programs without sample project
names. Generate, extract, and check them with:

```powershell
python semantic-model-verification/tools/reproduce_audit.py --codeinsight build-core/Release/codeinsight.exe --cases semantic-model-verification/audits/cpp-reproductions.json --output-dir build-core/source-audit/reproductions
```

Both commands return **1** when a semantic expectation fails. The original
2026-09-05 baseline is retained in `docs/SOURCE_MODEL_AUDIT.md`; current fixes and
results are described in `docs/PRODUCTION_HARDENING.md`.

The isolated programs are registered in CTest as
`semantic_model_audit_regressions` and run in both CI configurations. The table
checks require conservative address retention and forbid complete source-pattern
proofs, including for writes after a fill and conditional writes. The destructor
check compares distinct semantic override pairs because declarations and
out-of-line definitions each contribute observations. The sample check follows
the override chain through implicit intermediate destructors. The template
identity check counts the four source-declared primary/partial/explicit records
separately from newly retained concrete instantiations.

`codeinsight_cli_reliability` additionally checks malformed and extreme worker
counts, export self-overwrite rejection, failed-build publication preservation,
checkpoint recovery, old-schema invalidation, and persisted evidence validation.
C++ tests cover worker initialization failures, null factories, non-standard
exceptions, revision-owned property/type removal, SQLite reload and clean versus
incremental equivalence. These checks do not imply exhaustive C++ coverage.
