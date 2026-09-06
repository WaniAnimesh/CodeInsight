# Call resolution

The reusable API is `codeinsight/semantic/call_resolution.hpp`. Link only
`codeinsight_core`; no dead-code phase is required.

```cpp
#include "codeinsight/semantic/call_resolution.hpp"

auto model = codeinsight::SQLiteSnapshotStore{}.load_read_only("model.db");
for (const auto& call : codeinsight::summarize_call_targets(model)) {
    // call.observation identifies the original compiler call relationship.
    // call.targets contains known project symbols.
    // call.evidence identifies the original and derived call relationships.
    // call.completeness is Complete, Incomplete, or ExternalOnly.
}
```

`resolve_call_targets(model)` deterministically replaces the derived call edges
and bounded-table summaries while retaining compiler observations. It is also
used by incremental publication. It does not refresh entry-root policy: callers
changing observations should use the incremental build workflow to refresh roots,
validate, and publish the complete model.

## Observations and target sets

- Direct calls use compiler declaration identities. An exact static project
  call has a complete declaration target set; this does not prove a definition
  was indexed or that the containing TU is complete.
- Virtual calls are classified with `clang_Cursor_isDynamicCall` at the call
  expression. Explicitly qualified base calls stay static. A reverse override
  index and cache of transitive candidates serve repeated virtual calls.
  Open-world virtual sets remain incomplete; declared targets are retained.
- `CallableValueFlow` observes a local function-pointer storage symbol receiving
  a function address or another local pointer value. Its source is the destination
  storage, and its target is the function or copied storage. Initializers,
  assignments, function-to-pointer decay, parenthesized values, and conditional
  alternatives are supported. Taking an address is separately observed and does
  not create an invocation.
- `InvokesCallable` connects a caller to the precise local storage used by a
  dynamic call, with the same source range as its `Calls` observation. It is not
  itself a call to the storage symbol.
- Local propagation keys use canonical storage IDs and TU revisions, never
  variable spelling. The monotone work queue handles copies and cycles. Each
  derived target retains a separate conservative `Calls` edge. The original
  unresolved call remains, so known targets cannot be confused with a complete
  pointer analysis.
- Source-pattern table summaries provide conservative candidates only. They
  have an unknown index domain, incomplete targets and unproved escape safety.
  Every recognized stored callback remains a candidate. These hints never
  certify completeness or discharge an address-taken root. Source bytes must
  match the persisted file-version hash before this pass reads them.
- Referenced project class specializations and their callable members retain
  compiler USRs and semantic parents. `Instantiates`/`Specializes` edges link
  them to the compiler-reported primary; no name-based target substitution is
  used. Destructor overrides include implicit intermediate destructors.
- Automatic non-POD local objects and arrays record conservative `Destroys`
  dependencies at the declaration, using the compiler-selected object type and
  destructor when exposed. These are potential lifetime dependencies, not CFG
  exit events. Pointers, references, static and thread-local storage are excluded.
- An authoritative external static declaration, pruned from project symbols,
  produces `ExternalOnly`. An external virtual declaration or an unknown pointer
  does not prove that project targets are impossible.

The summary API indexes call relationships by caller, revision, and full source
range. It returns one result per compiler call observation, without flattening
TU evidence or build configurations. Evidence IDs refer to durable model records.
Call-target summaries are computed from persisted relationships and existing
bounded-flow artifacts; they do not require a new SQLite table.

## Complexity and boundaries

The local pass builds indexes once and sends each newly discovered target only
along copy constraints. With V storage nodes, E copy constraints, T distinct
function targets, C call sites, and R observed relationships, worst-case propagation performs
O(E*T) transfers plus ordered-map/set lookup costs. Propagation state occupies
O(E + V*T), call-site indexing O(R), and emitted candidates up to O(C*T).
This replaces source-wide address scans and name-based joins.
Virtual hierarchy traversal runs once per distinct declared target in a pass,
followed by unavoidable per-call candidate emission. No measured runtime or memory
speedup is claimed; cached closures trade memory for avoided repeated traversal.
The existing bounded-table pass still contains graph scans and source-pattern
matching and is not a general control-flow proof engine.

Local propagation is flow-insensitive: an assignment after a copy or call may
contribute an extra candidate. Incomplete sets do not discharge address roots.
Unknown writes, aliases, fields, globals, parameter/return flow, factories,
registries, arbitrary arrays, member-function pointers, captured closures, and
concrete virtual receiver narrowing remain outside this pass. They must not be
interpreted as having complete target sets. Existing template and implicit
callable observations are preserved, but this change does not expand those
mechanisms into whole-program analysis.

Schema 4, extraction version 15, canonicalization version 8 and flow version 3
invalidate older snapshots. `build` re-extracts them; inspect/export require a
current snapshot. Occurrences now persist type and property contributions.
Aggregation prefers definition types and unions boolean claims from active
observations; removing a revision removes its contributions. This does not
establish cross-TU ODR consistency or model conflicting declarations as valid.
See [Production hardening](PRODUCTION_HARDENING.md) for verification and limits.
