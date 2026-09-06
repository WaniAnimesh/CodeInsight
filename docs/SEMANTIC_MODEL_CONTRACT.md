# Semantic Model Contract

> **The Canonical Semantic Model is the product.**

> **No semantic conclusion may be stronger than the compiler evidence from which it was derived.**

CodeInsight represents compiler-observable C/C++ semantics for every indexed translation-unit revision. Authoritative inputs are workspace identity, normalized file versions, exact compilation commands, compiler identity, and extraction/canonicalization versions. Facts are immutable observations; canonical entities survive only while an active observation supports them.

Every entity and edge has deterministic identity, revision ownership, source provenance, evidence confidence, and `Exact`, `Conservative`, `Ambiguous`, or `Unresolved` resolution. Heuristic guesses do not enter the canonical model.

The initial supported environment is Windows x64, Clang/libclang 20+, C/C++ compilation databases, NTFS path case folding, and SQLite snapshots. A broken TU is isolated as `Partial` or `Failed`; project completeness is published with the snapshot. Consumers may demand `--require-complete`, which refuses publication unless every selected TU is `Complete`.

Compiler observations, derived conservative edges, target ownership, entry-root policy, and analysis conclusions remain distinct. Deduplicated exports never erase the underlying observations. A dead-code result is a candidate with an explicit confidence and reason, not authorization to delete source.

Schema v3 adds a non-destructive logical-symbol layer over configuration-specific raw symbols, authoritative target/dependency topology, unresolved target USR/spelling/domain/failure provenance, symbol visibility, and versioned indirect-call summaries. Schema-v2 snapshots remain readable through conservative analyzer-side logical grouping, but missing unresolved provenance and target closure permanently cap their confidence.

Structural publication and analysis readiness are independent contracts. `validate-model` proves storage, identity, ownership, reference, and lifetime invariants. Dead-code analysis separately proves whether coverage, targets, roots, cross-configuration bindings, project calls, and indirect dispatch are sufficient for a confidence band. A structurally valid model may therefore publish while high-confidence dead-code output remains disabled.
