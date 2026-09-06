# Static Analysis Tools That Can Be Built on CodeInsight

## Purpose

CodeInsight provides a compiler-backed, whole-project semantic model for C and C++. That model can become the shared foundation for a family of static-analysis tools instead of every tool independently reparsing source code and reconstructing symbol identity.

This document is a practical catalog of tools that can be built on top of CodeInsight. “All” is necessarily open-ended: new analyses become possible as the semantic model gains more facts, control-flow information, data-flow information, and build/runtime evidence. The catalog therefore separates tools that are realistic with the current model from tools that need additional extraction and tools that are longer-term research projects.

The expected shape of a future command is:

```powershell
codeinsight analyze --model model.db --analysis <analysis-name> --format sarif --output findings.sarif
```

The current executable exposes conservative dead-code analysis. The other names in this document are proposed analysis consumers, not promises that they already exist.

## Readiness labels

- **Current foundation** — the existing model already contains most of the required facts. A useful first implementation can be built as a model consumer.
- **Near-term extension** — the model has related facts, but needs targeted additions such as richer expressions, source ranges, configuration provenance, or a more complete call graph.
- **Data-flow extension** — requires control-flow graphs, value-flow facts, taint propagation, path sensitivity, or interprocedural summaries.
- **Research/production hardening** — technically possible, but needs substantial semantic coverage, benchmarking, false-positive controls, and operational maturity before it can be trusted.

## What the current model already offers

The strongest existing substrate is:

- canonical symbols across declarations, definitions, redeclarations, translation units, and configurations;
- compiler-backed type identity, aliases, qualifiers, arrays, pointers, references, function types, templates, specializations, and dependent constructs;
- source locations and translation-unit ownership;
- evidence counts and supporting configurations for semantic facts;
- symbol relationships such as calls, inheritance, overrides, object construction/destruction, and expression access where extracted;
- conservative candidate information for virtual and indirect call situations;
- compilation-database and target information;
- incremental SQLite snapshots, validation, diagnostics, exports, and relationship queries.

The model is not yet a complete C++ execution model. Analyses must preserve uncertainty and distinguish “proven,” “observed,” “candidate,” “not modeled,” and “not applicable.” A tool that silently treats missing semantic information as absence will produce unsafe conclusions.

## Analysis catalog

### 1. Model integrity and evidence tools

These tools improve trust in every downstream analysis.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Model validator | Check schema, foreign keys, identity invariants, relationship ownership, and persistence round trips. | SQLite schema and validation rules. | Current foundation |
| Snapshot health report | Summarize parsed, partial, failed, skipped, and invalidated translation units. | Translation-unit status and diagnostics. | Current foundation |
| Semantic completeness report | Report coverage by symbol kind, relationship, configuration, and call-target status. | Evidence counts, completeness flags, and configurations. | Current foundation |
| Stale model detector | Identify models whose source files, compiler flags, headers, or toolchain no longer match the snapshot. | File hashes, compilation commands, and toolchain identity. | Near-term extension |
| Evidence explorer | Show why a fact exists, which compiler invocation observed it, and which declarations support it. | Provenance and observation records. | Current foundation |
| Unsupported-construct report | List constructs that were encountered but are only partially modeled. | Diagnostics and extraction capability markers. | Near-term extension |
| Analysis confidence auditor | Detect findings based on incomplete translation units, unresolved calls, unknown macros, or incomplete lifetime facts. | Finding dependencies and uncertainty propagation. | Near-term extension |

### 2. Navigation and code-intelligence tools

These are high-value consumers because they can use canonical identity without requiring a full data-flow engine.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Symbol index | Search symbols by qualified name, kind, type, linkage, file, target, or configuration. | Canonical symbols and occurrences. | Current foundation |
| Definition/declaration finder | Navigate from a declaration to definitions and from a definition to all declarations. | Declaration-definition relationships. | Current foundation |
| Cross-reference explorer | Find callers, callees, readers, writers, users, derived classes, overrides, constructors, and destructors. | Canonical relationships. | Current foundation |
| Whole-project call graph | Generate direct-call graphs with evidence and unresolved/candidate edges. | Calls and call-target status. | Current foundation |
| Include/import graph | Show source/header dependencies and transitive include reachability. | Included files and compilation commands. | Near-term extension |
| Type graph browser | Explore aliases, canonical types, inheritance, pointee/reference relationships, and template specialization. | Type identity and type relationships. | Current foundation |
| Project topology map | Visualize target, namespace, directory, module, and dependency boundaries. | Targets, symbols, files, and relationships. | Near-term extension |
| API documentation index | Generate searchable API inventories and source-linked symbol documentation. | Public symbols, signatures, comments, and source locations. | Near-term extension |

### 3. Dead-code and reachability analysis

CodeInsight already has a conservative dead-code analysis consumer. The following are natural expansions, provided uncertainty is never mistaken for proof of deadness.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Unused function detector | Find functions with no known incoming references under selected targets/configurations. | Calls, exports, entry roots, linkage, and unresolved-call warnings. | Current foundation |
| Unused class/type detector | Find types with no meaningful uses, construction, inheritance, or exported visibility. | Type uses, construction, inheritance, and linkage. | Near-term extension |
| Unused member detector | Find fields and methods with no known reads, writes, calls, overrides, or reflective registration. | Expression access, calls, overrides, and registry evidence. | Near-term extension |
| Unused global/constant detector | Find variables and constants without observed reads or externally visible linkage. | Expression access and linkage. | Near-term extension |
| Unreachable target detector | Identify link targets or executable components with no configured roots or dependency path. | Target manifest, entry roots, and target graph. | Near-term extension |
| Dead-header detector | Find headers that are not included by selected targets or whose declarations are unused. | Include graph and symbol-use graph. | Near-term extension |
| Dead-branch detector | Find compile-time branches or runtime branches that are unreachable. | Macro/configuration facts and control-flow graph. | Data-flow extension |
| Unused template detector | Identify templates with no selected instantiations while respecting exports and dependent uses. | Template definitions, specializations, and call/type uses. | Near-term extension |
| Dynamic-registration risk report | Highlight apparently unused symbols that may be reached through registries, plugins, factories, callbacks, or reflection-like mechanisms. | Unknown indirect edges and registration evidence. | Current foundation as a warning; full analysis is research/production hardening |

### 4. Dependency and architecture analysis

These tools turn the semantic graph into enforceable design rules.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Include-what-you-use advisor | Find direct source uses that rely on transitive includes and suggest missing direct includes. | Include graph, declarations, and source uses. | Near-term extension |
| Include-cycle detector | Report direct and transitive include cycles and their target impact. | Include graph. | Near-term extension |
| Include fan-in/fan-out report | Find expensive or highly central headers. | Include graph and compile-time metadata. | Near-term extension |
| Layering rule checker | Enforce rules such as UI → service → domain → infrastructure. | Namespace, directory, target, include, call, and type edges. | Near-term extension |
| Forbidden-dependency checker | Reject dependencies from one component or package to another. | Target and relationship graph. | Near-term extension |
| Architecture drift detector | Compare the current dependency graph with an approved baseline or architecture manifest. | Versioned graph snapshots and rules. | Near-term extension |
| Dependency-cycle detector | Find cycles among libraries, targets, namespaces, modules, or packages. | Link and semantic dependency edges. | Current foundation for target graphs; broader forms need enrichment |
| Instability and coupling report | Compute afferent/efferent coupling, instability, centrality, and change-risk indicators. | Dependency graph and Git history integration. | Near-term extension |
| Hub and bottleneck detector | Identify symbols, headers, or services whose failure or change affects large portions of the graph. | Reachability and dependency centrality. | Near-term extension |
| Boundary violation detector | Find implementation details crossing public API, plugin, module, or ownership boundaries. | Visibility, linkage, target ownership, and use edges. | Near-term extension |

### 5. API, ABI, and compatibility analysis

These tools are valuable for libraries and SDKs where source compatibility is not enough.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Public API inventory | Produce the complete exported symbol/type/function inventory for a target. | External linkage, visibility, declarations, and target membership. | Current foundation with export rules |
| API diff checker | Compare two model snapshots and report added, removed, renamed, or changed public symbols. | Canonical identity and signature snapshots. | Near-term extension |
| Source compatibility checker | Find changes likely to break source consumers. | Types, overloads, defaults, templates, and visibility. | Near-term extension |
| ABI risk checker | Detect layout, vtable, calling-convention, alignment, visibility, and exception-ABI risks. | Type layout, inheritance, virtual methods, compiler ABI facts, and build settings. | Data-flow extension / research |
| ODR violation detector | Find conflicting definitions, incompatible declarations, or duplicate entities across translation units. | Declaration/definition identity and configuration evidence. | Near-term extension |
| Missing-definition detector | Find referenced functions, variables, templates, or vtable-related entities without a definition in the selected targets. | References, definitions, linkage, and target closure. | Near-term extension |
| Override consistency checker | Report suspicious override sets, hidden overloads, missing `override`, and inconsistent virtual contracts. | Inheritance, override, signatures, and visibility. | Current foundation for a focused version |
| Deprecation impact analyzer | Show consumers of deprecated APIs and estimate migration scope. | API metadata, attributes, and reverse references. | Near-term extension |
| Versioning policy checker | Enforce semantic-versioning rules against model diffs and public API manifests. | API diff plus release policy. | Near-term extension |

### 6. C++ correctness and type-safety tools

These tools range from graph checks that are feasible now to path-sensitive analyses that require a control-flow engine.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Declaration/signature mismatch checker | Detect inconsistent declarations, definitions, qualifiers, ref-qualifiers, and overload identity. | Compiler-resolved declarations and types. | Current foundation |
| Suspicious implicit-conversion checker | Flag narrowing, signedness, qualification, pointer, and user-defined conversion risks. | Expression types and conversion facts. | Near-term extension |
| Unsafe cast checker | Find C-style casts, reinterpret casts, downcasts, and unchecked conversions. | Cast expressions and type relationships. | Near-term extension |
| Nullability checker | Track nullable pointers/references and warn on unsafe use. | Types, annotations, flow, and dereference expressions. | Data-flow extension |
| Uninitialized-use checker | Detect reads before definite initialization. | Control-flow and value-flow facts. | Data-flow extension |
| Bounds checker | Detect possible out-of-bounds array, pointer, iterator, and span access. | Expressions, ranges, points-to facts, and path conditions. | Research/production hardening |
| Boolean/condition correctness checker | Find constant conditions, tautologies, impossible comparisons, and suspicious precedence. | Expression trees and constant evaluation. | Near-term extension |
| Switch exhaustiveness checker | Detect missing enum cases and unsafe default behavior. | Enum identity and control-flow facts. | Near-term extension |
| Virtual-dispatch safety checker | Report incomplete override sets, slicing risks, non-virtual destructor hazards, and candidate dispatch ambiguity. | Inheritance, overrides, constructors/destructors, and call candidates. | Near-term extension |
| Lifetime/ownership checker | Model ownership transfer, borrowing, destruction, escape, and use-after-lifetime risks. | Construction/destruction, references, pointer flows, and annotations. | Data-flow extension |
| RAII misuse checker | Detect manual resource handling that bypasses or conflicts with RAII conventions. | Object lifecycle, resource APIs, and control-flow. | Near-term extension |
| Exception-safety checker | Analyze leaks, partial updates, noexcept contracts, and exception paths. | Call graph, object lifecycle, CFG, and exception specifications. | Data-flow extension |
| Template correctness checker | Find unsafe specialization gaps, dependent-call assumptions, and inconsistent constraints. | Templates, specializations, dependent relationships, and types. | Near-term extension |

### 7. Resource and systems-programming analysis

These tools are especially relevant for native code, but they need a library model and path-sensitive facts to avoid overwhelming users with false positives.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Memory leak detector | Find allocations without a matching release on all relevant paths. | Allocation/free summaries, ownership, and CFG. | Data-flow extension |
| Double-free detector | Find possible repeated destruction or release. | Resource state and path-sensitive flow. | Data-flow extension |
| File/socket/handle leak detector | Track operating-system resources across returns, exceptions, and callbacks. | Resource API summaries and CFG. | Data-flow extension |
| Lock misuse detector | Find missing unlocks, unlocks on the wrong lock, and inconsistent lock guards. | Lock API summaries and CFG. | Data-flow extension |
| Iterator invalidation checker | Detect use of iterators or references after container mutation. | Container summaries, aliasing, and flow. | Research/production hardening |
| String lifetime checker | Detect dangling `string_view`, `span`, references, and pointers to temporary or local storage. | Type identity, lifetime, escape, and flow. | Data-flow extension |
| Move-state checker | Find use-after-move and suspicious repeated moves. | Move operations and value-state flow. | Data-flow extension |
| System-call error checker | Enforce checking of return values from selected OS/POSIX/library calls. | Call graph, expression uses, and API summaries. | Near-term extension |
| Error-code propagation checker | Find ignored, overwritten, or inconsistently propagated error results. | Return values, call sites, and flow. | Data-flow extension |
| Resource-budget analyzer | Estimate peak open files, handles, allocations, or threads per target. | Resource summaries and call paths. | Research/production hardening |

### 8. Security analysis

Security analyses must be explicit about trust boundaries, sanitizers, source/sink models, and uncertainty. Processing arbitrary source paths also requires external sandboxing; a model consumer does not make extraction safe by itself.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Dangerous API checker | Flag banned or risky APIs such as unsafe string, process, filesystem, or cryptographic calls. | Call sites and configurable API rules. | Near-term extension |
| Taint analysis | Track attacker-controlled data to sensitive sinks. | CFG, interprocedural value flow, source/sink/sanitizer rules. | Data-flow extension |
| Command-injection analyzer | Find untrusted data reaching process execution or shell APIs. | Taint flow and API summaries. | Data-flow extension |
| Path-traversal analyzer | Find untrusted paths reaching filesystem operations without canonicalization or policy checks. | Taint flow and filesystem API summaries. | Data-flow extension |
| SQL/injection analyzer | Detect unsafe construction of database, query, or interpreter commands. | Taint flow and sink models. | Data-flow extension |
| Format-string checker | Detect non-literal or mismatched format arguments. | Call signatures, string literals, and argument types. | Near-term extension |
| Cryptography policy checker | Detect weak algorithms, insecure modes, hard-coded parameters, and missing verification. | Calls, constants, and configurable policy. | Near-term extension |
| Secret-in-source detector | Find likely credentials, tokens, private keys, and connection strings. | String literals, file metadata, and entropy/pattern rules. | Current foundation as lexical tooling; semantic integration is near-term |
| Authentication/authorization boundary checker | Verify sensitive operations are guarded by approved checks. | Call graph, annotations, and policy rules. | Data-flow extension |
| Deserialization safety checker | Identify unsafe deserialization and missing validation. | API calls, types, and taint flow. | Data-flow extension |
| Supply-chain usage report | Inventory external headers, libraries, compiler runtimes, and reachable third-party APIs. | Compilation commands, includes, link targets, and manifests. | Near-term extension |

### 9. Concurrency and parallelism analysis

Concurrency analysis is one of the hardest areas because correctness depends on runtime scheduling, aliasing, atomics, and library semantics.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Thread-entry inventory | List thread creation sites, callbacks, executors, tasks, and process entry points. | Calls, known concurrency APIs, and target roots. | Near-term extension |
| Shared-state access report | Find fields or globals accessed from multiple thread contexts. | Expression access, call graph, and thread summaries. | Data-flow extension |
| Data-race detector | Find conflicting unsynchronized reads and writes. | Alias analysis, locksets, atomics, and thread flow. | Research/production hardening |
| Deadlock detector | Find lock-order cycles and inconsistent acquisition patterns. | Lock acquisition graph and path summaries. | Data-flow extension |
| Lock-guard consistency checker | Verify that selected accesses occur under required guards. | Access graph and lock annotations. | Data-flow extension |
| Atomicity checker | Find non-atomic compound operations or incorrect memory-order assumptions. | Atomic expressions, CFG, and concurrency summaries. | Research/production hardening |
| Callback lifetime checker | Detect asynchronous callbacks retaining or using destroyed objects. | Callback registration, captures, ownership, and lifetime flow. | Research/production hardening |
| Thread-safety annotation checker | Enforce annotations such as guarded-by, requires-lock, and locks-excluded. | Attributes and access relationships. | Near-term extension |

### 10. Performance and scalability analysis

Static performance tools should report opportunities and risk indicators, not pretend to know actual runtime cost without profiling data.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Allocation-site inventory | Find allocation-heavy APIs, per-call allocations, and allocations in loops. | Calls, control-flow, and API summaries. | Near-term extension |
| Copy/move cost checker | Detect expensive copies, pass-by-value risks, and missed moves. | Types, expressions, overloads, and flow. | Near-term extension |
| Temporary-object checker | Find avoidable temporaries and lifetime extensions. | Expression trees and object lifecycle. | Near-term extension |
| Virtual-dispatch hotspot report | Identify high-fan-out or frequently reachable virtual call sites. | Call candidates and reachability. | Current foundation as structural report; runtime weighting needs profiles |
| Inline/cold-path candidate report | Identify tiny hot-looking functions, large functions, and cold error paths. | Function size, call graph, and optional profiles. | Near-term extension |
| Algorithmic complexity checker | Detect known quadratic or worse patterns in loops and container operations. | CFG, loop nesting, types, and API summaries. | Data-flow extension |
| Header compile-cost analyzer | Find headers with large transitive closure and configuration-sensitive parse cost. | Include graph and build timings. | Near-term extension |
| Template-instantiation risk report | Find broad or repeated template instantiation patterns. | Template and configuration facts. | Near-term extension |
| Cache/locality risk checker | Identify pointer-heavy layouts, false-sharing risks, and poor data locality patterns. | Type layout, access patterns, and annotations. | Research/production hardening |
| Static capacity estimator | Estimate graph size, memory pressure, and worst-case analysis cost before a full run. | Translation-unit and semantic-model statistics. | Near-term extension |

### 11. Code-quality and maintainability analysis

These are useful for prioritization, but they should complement compiler diagnostics rather than duplicate style-only tools.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Complexity report | Report function, class, namespace, and target complexity. | AST/control-flow structure. | Near-term extension |
| Large-function/class detector | Find oversized units with high coupling or many responsibilities. | Source ranges, symbols, and relationships. | Near-term extension |
| Duplicate implementation detector | Find structurally or semantically similar functions and types. | Normalized AST or token fingerprints. | Near-term extension |
| Change-impact score | Rank symbols by dependency centrality, churn, and historical risk. | Model graph plus Git history. | Near-term extension |
| Code ownership gap report | Find critical files, targets, or APIs without declared owners. | Paths, targets, and CODEOWNERS/policy files. | Current foundation as repository integration |
| Documentation coverage checker | Find public APIs without comments, examples, or usage documentation. | Public API inventory and comments. | Near-term extension |
| Naming consistency checker | Detect inconsistent names across related symbols and APIs. | Canonical symbol families and configurable conventions. | Near-term extension |
| API cohesion report | Find classes or namespaces whose public members have weak internal cohesion. | Member usage and relationship graph. | Research/production hardening |
| Technical-debt hotspot report | Combine complexity, churn, defects, dependency centrality, and missing tests. | Model, Git, issue tracker, and CI integrations. | Near-term extension |

### 12. Modernization and refactoring tools

CodeInsight can provide the semantic inventory and impact analysis needed to make automated refactoring safer. Source rewriting still requires a precise edit engine and review workflow.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Safe rename engine | Rename a canonical symbol across declarations, definitions, and references. | Canonical identity and source ranges. | Near-term extension |
| Include migration tool | Replace transitive includes with direct includes and remove unused includes. | Include/use graph. | Near-term extension |
| API deprecation migrator | Find consumers and generate migration plans or edits. | API inventory and reverse references. | Near-term extension |
| Raw-pointer modernization advisor | Suggest ownership-aware smart-pointer or reference alternatives. | Type, lifetime, and ownership evidence. | Data-flow extension |
| C++ standard migration checker | Find constructs that block C++14/17/20/23 migrations or can be modernized. | AST, types, and compiler configuration. | Near-term extension |
| Override/`final` advisor | Add explicit intent markers where safe and useful. | Virtual/override graph. | Near-term extension |
| Namespace/module migration planner | Group symbols and includes into candidate module or package boundaries. | Dependency and ownership graph. | Research/production hardening |
| Generated-code boundary checker | Prevent edits to generated files and identify source-of-truth files. | File metadata and build rules. | Near-term extension |
| Refactoring blast-radius preview | Show affected files, targets, APIs, tests, and configurations before an edit. | Reverse dependency and configuration graphs. | Current foundation with a focused UI/CLI |

### 13. Build, configuration, and portability analysis

Compilation databases make build behavior a first-class analysis input.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Compilation-database validator | Detect missing files, invalid compiler paths, duplicate commands, and inconsistent working directories. | Compilation database records. | Current foundation |
| Configuration drift detector | Compare debug/release, platform, feature, and target configurations. | Multiple compilation databases and evidence sets. | Current foundation |
| Macro-variant analyzer | Find symbols whose presence, type, or relationship changes by macro configuration. | Preprocessor/configuration provenance. | Near-term extension |
| Portability checker | Identify platform-specific headers, APIs, types, assumptions, and conditional paths. | Include graph, macros, target settings, and API rules. | Near-term extension |
| Toolchain compatibility checker | Report use of compiler-specific extensions, version-sensitive behavior, and unsupported frontend features. | Compiler flags, language mode, and AST facts. | Near-term extension |
| Reproducibility checker | Compare commands, toolchain versions, generated headers, and path dependencies across machines. | Build metadata and environment manifests. | Near-term extension |
| Target leakage detector | Find files or symbols compiled into unintended targets. | Target partitions and compilation records. | Current foundation |
| Build-impact estimator | Estimate which translation units and targets need rebuilding after a change. | Include and semantic dependency graph. | Near-term extension |
| Feature-flag inventory | Track flags, macros, and configuration branches and their observed usage. | Configuration facts and source ranges. | Near-term extension |

### 14. Testing and verification tools

These tools connect static structure to test assets. They should not be confused with runtime code coverage unless runtime coverage data is supplied.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| Test-to-code mapper | Map test cases and test targets to production symbols and relationships. | Test metadata, target graph, and calls. | Near-term extension |
| Test impact analyzer | Select tests likely affected by a code change. | Reverse dependency and target graphs. | Near-term extension |
| Untested public API report | Find public symbols with no mapped tests or examples. | API inventory and test mapping. | Near-term extension |
| Contract-test candidate finder | Find external boundaries where API/schema compatibility tests should exist. | Calls, adapters, and public interfaces. | Near-term extension |
| Assertion quality analyzer | Find tests with weak assertions, unreachable setup, or unverified return values. | Test AST and control-flow. | Data-flow extension |
| Fixture gap analyzer | Compare semantic construct coverage against the model’s known limitations. | Fixture manifests and capability matrix. | Current foundation |
| Fuzz-target candidate finder | Identify parsers, deserializers, protocol handlers, and complex boundary functions suitable for fuzzing. | API and call graph rules. | Near-term extension |
| Mutation-risk prioritizer | Rank locations where mutations are unlikely to be detected by the current test set. | Test mapping plus mutation results. | Research/production hardening |

### 15. Reporting, CI, and developer-experience tools

The same analyses become much more useful when they can run incrementally and report only actionable regressions.

| Tool | Purpose | Primary evidence | Readiness |
| --- | --- | --- | --- |
| SARIF exporter | Publish findings to code-scanning systems and pull-request annotations. | Any analysis finding with source ranges and severity. | Near-term extension |
| Baseline/regression checker | Fail CI only for new or worsened findings. | Versioned findings and model snapshots. | Near-term extension |
| Pull-request impact report | Summarize changed APIs, callers, targets, configuration variants, and risk. | Git diff plus semantic graph. | Near-term extension |
| IDE navigation service | Provide symbol search, definitions, references, call hierarchy, and diagnostics. | SQLite model and source locations. | Current foundation as a service layer |
| Architecture dashboard | Visualize dependencies, cycles, ownership, and hotspots over time. | Graph exports and historical snapshots. | Near-term extension |
| Quality gate runner | Apply project-specific policies to API, security, architecture, and model completeness. | Analysis results plus policy files. | Near-term extension |
| Review assistant | Generate a machine-readable review checklist from affected semantic areas. | Diff impact and policy configuration. | Near-term extension |
| Finding suppressions manager | Store justified suppressions with owners, expiry dates, and evidence. | Finding identity and repository policy. | Near-term extension |
| Multi-repository graph service | Join models from related repositories while preserving ownership boundaries. | Exported model contracts and repository metadata. | Research/production hardening |

## Recommended implementation order

The highest-value sequence is to increase trust before increasing analysis breadth:

1. Model health, evidence explorer, and semantic completeness reports.
2. Symbol index, cross-reference explorer, call graph, and include graph.
3. API inventory, API diff, ODR checks, missing-definition checks, and override checks.
4. Architecture rules, dependency cycles, target leakage, and change-impact reports.
5. Dead-code extensions with explicit dynamic-registration warnings.
6. SARIF output, baselines, suppressions, and CI quality gates.
7. Targeted lifetime, resource, and error-propagation analyses.
8. CFG/value-flow infrastructure for nullability, taint, concurrency, and bounds analysis.
9. Profile-assisted performance and runtime/static correlation.

This order creates useful tools while steadily reducing the risk that incomplete semantic extraction is hidden behind confident-looking findings.

## Shared requirements for production-quality analyzers

Every analysis built on this project should:

- state its semantic prerequisites and known blind spots;
- preserve evidence and source locations for every finding;
- represent uncertainty explicitly rather than treating unknown as safe or dead;
- support configuration and target scoping;
- provide deterministic output for the same model and rule set;
- expose machine-readable output, preferably JSON and SARIF;
- support baselines, suppressions, severity, confidence, owner, and remediation text;
- avoid mutating source code unless the user explicitly requests a separate rewrite operation;
- be tested against positive, negative, ambiguous, incomplete, and multi-configuration fixtures;
- measure precision, recall, runtime, memory, and false-positive rate on representative real-world corpora;
- fail closed for safety-sensitive conclusions when required evidence is missing;
- record the model schema version, tool version, rule-set version, compiler identity, and configuration set used to produce the result.

## Example future analysis commands

These examples describe a possible stable interface; the named analyses are not all implemented today.

```powershell
# Architecture and dependency checks
codeinsight analyze --model model.db --analysis dependency-cycles --format sarif --output dependency-cycles.sarif
codeinsight analyze --model model.db --analysis architecture-rules --rules architecture.yaml --format json --output architecture.json

# API and compatibility checks
codeinsight analyze --model model.db --analysis public-api --target app --format json --output api.json
codeinsight analyze --model model.db --analysis api-diff --baseline previous.db --format sarif --output api-diff.sarif

# Resource and correctness checks
codeinsight analyze --model model.db --analysis missing-definitions --format records
codeinsight analyze --model model.db --analysis lifetime --rules lifetime.yaml --format sarif --output lifetime.sarif

# CI regression checks
codeinsight analyze --model model.db --analysis all-enabled --baseline findings.json --fail-on new --format sarif --output findings.sarif
```

## Important boundary

A semantic graph is necessary for serious C++ analysis, but it is not sufficient for every analysis. Call graphs do not prove runtime reachability; declaration identity does not prove lifetime safety; type identity does not prove value ranges; and a complete compilation database does not prove that all generated, loaded, or reflected behavior was observed.

The current project should first be treated as a platform for evidence-backed analysis experiments and narrowly scoped engineering tools. Broader security, concurrency, memory-safety, and performance claims require additional semantic layers, carefully validated summaries, real-world corpora, fuzzing, benchmarking, and operational governance. See [shortcomings.md](shortcomings.md) for the project’s current maturity assessment.
