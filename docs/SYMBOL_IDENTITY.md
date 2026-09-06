# Symbol Identity

Canonical identity uses a non-empty Clang USR when it is unambiguous. Children are canonicalized only after their semantic parents, and both strong and fallback child keys contain the canonical parent ID. The fallback key also contains workspace, symbol kind, qualified name, canonical TypeIDs, configuration, and linkage scope. If a parent cannot be resolved uniquely, that unresolved state is explicit in the key; conflicting canonical parents are rejected rather than silently overwritten.

Internal-linkage symbols, anonymous namespaces, locals, and lambdas include physical file/scope identity. Overloads include signature and method qualifiers. SHA-256 IDs are stored beside full keys; unique constraints and in-memory checks detect collisions.

Symbols are entities; declarations and definitions are revision-owned occurrences. A symbol is deleted only after its final supporting observation disappears.

Schema v3 preserves that configuration-specific raw identity and adds `LogicalSymbol`. A logical ID is derived from workspace, non-empty Clang USR, and linkage domain without the build configuration. Internal and anonymous-namespace groups also include physical source-file identity. Raw variants are never destructively merged. Reachability instead projects bidirectional conservative equivalence edges across compatible variants, while reporting groups all definition-bearing variants under one logical candidate. Fallback symbols without reliable USRs remain separate.

Process-entry definitions include defining-file scope in their raw key. Consequently `AtlasCli::main` and `AtlasTests::main` remain distinct even when one umbrella compilation database contains both executables.
