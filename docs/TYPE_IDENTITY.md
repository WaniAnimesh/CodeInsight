# Type Identity

Types are structural nodes: builtin, named, pointer, lvalue/rvalue reference, array, function, member pointer, template specialization, auto, decltype, dependent, or unknown. CV qualifiers belong to the qualified node.

Canonical keys encode node kind, qualifiers, referenced symbol, child types, extent, variadic/calling-convention state, and template arguments. Spelled and canonical spellings are retained for provenance; spelling is never primary identity. Unknown and dependent states are explicit.
