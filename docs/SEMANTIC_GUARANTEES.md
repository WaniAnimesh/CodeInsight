# Semantic Guarantees

Identical workspace bytes, ordered commands, compiler/extraction versions, and configuration produce semantically identical normalized snapshots independent of scheduling and enumeration order.

Published snapshots guarantee collision-checked IDs, valid endpoints for resolved edges, occurrence ownership, file-version-backed ranges, TU/configuration integrity, overload separation, linkage isolation, and no unsupported canonical orphans. Completeness counts complete, warning, partial, and failed TUs. Unsupported compiler constructs remain explicit dependent, unknown, or unresolved evidence.

Publication is fail-closed. The in-memory model is semantically validated before a temporary SQLite snapshot is written transactionally. That staged database must pass SQLite integrity and foreign-key checks, reopen read-only with matching schema/extraction/canonicalization versions, pass semantic validation after deserialization, and normalize identically to the source model. Only then is it atomically promoted; any failure leaves the previously published snapshot untouched.

Build-target memberships and entry roots have collision-checked identities and validated endpoints. Semantic exports are deterministic and provenance-preserving. Virtual-dispatch expansion and address-taken roots are conservative: they may retain extra code, but they must not be represented as exact runtime dispatch.
