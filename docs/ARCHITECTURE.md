# Architecture

> **The Canonical Semantic Model is the product.**

> **No semantic conclusion may be stronger than the compiler evidence from which it was derived.**

```text
workspace + compile_commands.json
  -> CompilationRequest
  -> ITranslationUnitFrontend / LibClangFrontend
  -> immutable FactBatch
  -> Canonicalizer (single deterministic commit authority)
  -> resolve_call_targets (compiler-independent derived call edges)
  -> SemanticModelValidator
  -> SQLiteSnapshotStore (atomic publication)
```

Extraction is bounded and parallel. Batches are sorted by TU identity before one thread mutates the model. Declarations and macros are emitted only from component-safe project roots; includes from project-owned files retain their external targets, while external declarations remain unresolved evidence rather than being promoted to project symbols. libclang `CX*` types stop inside `src/libclang_frontend.cpp`.

The C++ model is authoritative. SQLite adapts through normalized workspace, configuration, file/version, TU/revision, symbol/occurrence, type, relationship, macro, include, diagnostic, and provenance tables. Incremental execution loads the prior snapshot, finds content/command changes plus reverse include closure, replaces affected observations, garbage collects, validates, and republishes.

Schema v4 stores occurrence-owned types and properties as well as build targets, TU target membership, compiler-observed exports, and entry roots. Repeated compilation databases are merged without flattening configurations. Address-taken functions remain conservative roots. Current local and table-candidate passes do not establish complete indirect target sets. Virtual calls retain the exact declared target and receive separately marked `Conservative` derived edges to the transitive override set.

The raw observation graph remains authoritative. The semantic export is a deterministic projection that groups equivalent edges and reports evidence multiplicity and configurations. Reachability analysis consumes roots and resolved or conservative call/lifecycle edges; it never upgrades derived evidence to compiler-exact evidence.

Call-resolution policy lives in `src/call_resolution.cpp`, behind
`codeinsight/semantic/call_resolution.hpp`. The incremental executor orchestrates
extraction, resolution, roots, and publication; it does not implement call-target
inference. Compiler facts stay immutable. Resolution can also be rerun directly
on a loaded in-memory model.

`codeinsight_analysis` depends on `codeinsight_core`; the core does not depend on
analysis APIs or candidate reporting. `CODEINSIGHT_BUILD_ANALYSIS=OFF` excludes
that consumer and its CLI command. Entry roots and call summaries remain generic
semantic facts. The standalone conformance runner links only the core.