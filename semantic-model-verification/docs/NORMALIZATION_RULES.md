# Normalization Rules

Normalization removes database row order, generated IDs, transaction state, timestamps, temporary workspace prefixes, insertion order, and scheduling order. It preserves symbol kind, qualified name, linkage, canonical type spelling, occurrence roles, relationship kind and endpoints, resolution, dispatch, access, virtual/dependent flags, diagnostics, and TU completeness.

Symbols are sorted by a semantic key. Internal, local, and no-linkage symbols add supporting TU paths so same-spelled entities from different translation units remain distinct. Relationships and diagnostics use deterministic lexical ordering. Duplicate normalized rows remain duplicates; normalization never silently deduplicates semantic evidence.

Golden snapshots are line-oriented normalized semantics, not raw database exports. A semantic diff reports missing and unexpected rows with multiplicity.
