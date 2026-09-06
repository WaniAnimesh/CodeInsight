# Analysis and confidence

CodeInsight separates compiler observations, conservative derived facts, and analyzer conclusions. This document describes the optional `codeinsight_analysis` consumer, enabled by `CODEINSIGHT_BUILD_ANALYSIS`. The core builds and operates without it.

## Entry roots

Roots are generated per build configuration from process entry functions (`main`, `wmain`, `WinMain`, and `wWinMain`), compiler-observed `dllexport` declarations, explicit `--entry-root` values, and address-taken functions. Externally linked callable definitions are retained as open-world API roots until visibility/export provenance can prove them target-private. Local function-pointer initializers, assignments, copies, decay, and conditional values produce conservative dynamic call candidates; their target sets remain incomplete. Address-taking remains a root for flows through assignments, fields, parameters, returns, or aliases that are not resolved completely.

Build targets come from compilation-database inputs. Use `--target-kind executable` or `--target-kind test` only for closed-world targets. Libraries remain open-world because consumers outside the indexed workspace may call externally linked symbols.

## Dead-code candidates

`codeinsight analyze --analysis dead-code` traverses exact and conservative calls, construction, destruction, containment, logical-symbol equivalence, template retention, virtual candidates, and proven indirect targets from known roots. Candidates are grouped at logical-definition level and emitted only when no definition-bearing variant is reachable. The numeric ranking never overrides readiness gates: high confidence requires complete TUs, authoritative target closure and roots, complete cross-configuration binding, no reachable project-owned unresolved call, and no unresolved indirect dispatch. Every suppressed confidence band names the failed gates.

Schema-v2 snapshots infer conservative same-USR logical groups for external symbols. Because their unresolved calls lack reliable provenance, any reachable unclassified call disables high confidence. Structural validity and dead-code readiness are separate results.

The output is evidence for review, not an automatic deletion instruction.

## Observation and semantic views

The observation view retains every revision-owned edge. Header facts may therefore appear once per translation unit. The semantic view groups equivalent edges while preserving evidence count and supporting configuration IDs. Use the semantic view for graph construction and the observation view for source-level explanation.

## Flow-sensitive analysis boundary

The base snapshot records symbol, type, expression-access, lifecycle, call, hierarchy, configuration, target, and root facts. CodeInsight additionally persists narrowly scoped `IndirectCallSummary` artifacts keyed by call observation, TU revision, configuration, and flow-analyzer version.

The table candidate pass recognizes a narrow source pattern but does not prove C++ execution order, index semantics, or escape safety. It reports every recognized stored callback as a conservative candidate, marks the summary incomplete with an unknown domain, and preserves all address-taken roots. Complete table proofs from earlier versions were unsound and are invalidated by the current versions. A future compiler-backed control-flow analysis is required before these roots can be discharged.
