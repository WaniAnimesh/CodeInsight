# Revision and Lifetime

Every observation belongs to one TU revision. Updating a TU detaches the old revision, ingests a new immutable batch, canonicalizes, garbage collects unsupported entities, validates, and commits.

Source/header content, command, configuration, extractor version, or dependency changes invalidate affected TUs. Compiler-observed include edges drive transitive reverse invalidation. Deleted source TUs disappear; entities supported elsewhere remain.

Snapshots are written to a sibling temporary SQLite file, validated, then atomically published. Read-only opens never create schema or mutate state.
