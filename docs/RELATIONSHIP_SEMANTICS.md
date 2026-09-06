# Relationship Semantics

Relationships are typed observations with source, optional canonical target, evidence range, TU revision, resolution, dispatch, origin, target domain, and resolution-failure class. An unresolved relationship retains `observed_target_usr` and `observed_target_spelling`; loss of canonical resolution therefore does not erase whether Clang observed a project, external, dependent, indirect, or unknown target.

Direct compiler-resolved calls are exact. Virtual calls store the declared target with virtual dispatch, not a claimed runtime receiver. Dependent calls and function-pointer calls without supported local initializer evidence remain unresolved. Inheritance keeps access, virtual, and dependent properties; only direct override edges are compiler-observed. Compiler-observed includes are semantic truth.

`Reads` and `Writes` distinguish value use from mutation; compound assignment and increment/decrement emit both. Initialization emits a write. `Constructs` covers constructor calls and allocation expressions, `Destroys` covers destructor calls and delete expressions, and `TakesAddress` records explicit address-of expressions. Generic `References` remain available independently.

Local function-pointer initializers, assignments, copies, decay, and conditional alternatives emit `CallableValueFlow` observations. `InvokesCallable` identifies the exact storage at a call site. A revision-scoped propagation pass adds separate `Conservative`, `Dynamic` call candidates while retaining the unresolved compiler call. These target sets remain incomplete. Fields, parameters, returns, and aliases remain unsupported; address roots are retained. See [CALL_RESOLUTION.md](CALL_RESOLUTION.md) for the API and limits.

After canonicalization, a virtual call's exact declared target is preserved and transitive override candidates are emitted as `Calls` with `Conservative`, `Virtual`, and `DerivedCanonicalization`. These candidate edges support safe reachability but are not claims about the runtime receiver.

Explicitly qualified base calls are static, even when the named method is virtual. Dispatch classification comes from the compiler call expression.

When Clang resolves a call to a project-owned implicit constructor, destructor, conversion, or method that it did not enumerate as a declaration cursor, the frontend synthesizes an implicit symbol observation for that resolved callable. Template declarations and specializations retain separate compiler identities and flags; dependent template uses remain explicitly dependent instead of being guessed.

Reachable concrete function-template instantiations retain their source primary. Class-template member variants are conservatively related by qualified name and complete callable-USR suffix, which preserves overload distinctions. These are analysis-projection edges, not rewritten compiler observations.
