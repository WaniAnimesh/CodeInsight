# Compilation Model

A TU is `(source FileID, BuildConfigurationID)`. A revision adds source content hash, compilation fingerprint, and extraction version.

`compile_commands.json` is authoritative. Every request retains its exact original argument vector for provenance and a separately derived semantic argument vector for frontend execution and identity. Wrapper launchers are resolved to the real compiler, response files are expanded recursively with cycle and depth protection, compiler identity includes the resolved executable's stable metadata, and source/output/dependency-emission operands are excluded from configuration identity. Output-only command changes therefore change invocation provenance without creating a false configuration.

Multiple `--compile-commands` inputs may be supplied. Commands are deduplicated by `(source, BuildConfigurationID)`; conflicting semantic argument vectors are rejected. Without additional topology, each input is only a provisional build-target boundary. `--target` supplies a display name, `--target-kind` supplies its kind, and `--authoritative-target` is valid only when that input is one real link product.

For an umbrella compilation database, use `--target-manifest FILE`. The UTF-8, line-oriented format is:

```text
# CodeInsight target manifest v1
target|AtlasCore|static-library
target|AtlasCli|executable
target|AtlasTests|test
member|AtlasCore|src/model.cpp
member|AtlasCli|app/main.cpp
member|AtlasTests|tests/test_main.cpp
depends|AtlasCli|AtlasCore
depends|AtlasTests|AtlasCore
```

Member paths are resolved against the workspace. Every selected TU must be assigned; unknown targets, missing commands, duplicate target names, and unassigned TUs reject the build. Manifest targets are authoritative. The target graph is closed only when every target is reachable as an executable/test or one of its transitive dependencies. A compilation database by itself never proves link topology.

CodeInsight requires Clang 20+. Configure-time and runtime version checks must agree, and Windows builds deploy the selected `libclang.dll` beside the executable. clang-cl editor-only unknown-argument warnings injected by libclang are suppressed because they do not alter semantic parsing; all other diagnostics remain model evidence.

The configuration key covers compiler identity, inferred language, and the ordered, path-normalized semantic arguments. The compilation fingerprint is derived from that key. Project roots form a separate extraction-view fingerprint: changing a boundary invalidates the affected TU without changing its BuildConfigurationID. The default project root is the workspace; repeated `--project-root` values may admit additional source trees.

Windows paths are absolute, lexically normalized, slash-normalized, and case-folded. Existing paths are weakly canonicalized when possible. Generated and external files receive normal immutable versions.
