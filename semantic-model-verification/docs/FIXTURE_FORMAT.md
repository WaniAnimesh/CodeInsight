# Fixture Format

A fixture directory contains `fixture.json` plus all source and header files for one miniature project. The manifest requires `name`, `category`, and `sources`; it may specify `language`, `standard`, `include_paths`, `defines`, and ordered `compiler_arguments`.

`expect` supports translation-unit counts, symbol presence and absence, occurrence counts, type kind and canonical spelling, function/template/variable flags, relationships and their resolution/dispatch/access properties, negative relationships, and canonical type equivalence.

Paths are relative to the fixture directory. The runner copies the complete fixture to an isolated temporary workspace and creates an authoritative compilation database there. Fixture names are semantic, globally unique, and stable.

Example:

```json
{
  "name": "direct_call",
  "category": "relationships",
  "standard": "c++20",
  "sources": ["src/main.cpp"],
  "expect": {
    "symbols": [{"qualified_name": "caller", "kind": "Function", "definitions": 1}],
    "relationships": [{"source": "caller", "kind": "Calls", "target": "target", "resolution": "Exact"}]
  }
}
```
