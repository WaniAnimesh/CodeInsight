# Semantic Assertions

Declarative assertions currently cover:

- exact symbol count by qualified name and optional `SymbolKind`;
- declaration and definition occurrence counts;
- `TypeKind`, canonical type spelling, and canonical type equivalence;
- callable, variable, and template properties;
- exact relationship count with optional target, resolution, dispatch, access, virtual, and dependent constraints;
- explicit absence of symbols and relationships;
- total and failed translation-unit counts.

All counts are exact. This makes accidental duplicate canonical entities visible. Query helpers return all matches rather than selecting an arbitrary symbol. Failure messages include the selector, expected value, and observed count or value.
