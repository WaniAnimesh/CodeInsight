# Conformance Findings

## Seed run - 2026-08-09

The initial 30-fixture suite produced 29 passes and one semantic failure on Windows x64 with the configured libclang toolchain.

### Explicit class-template specialization is classified as instantiation

Fixture: `templates/explicit_specialization`

Expected semantic edge:

```text
Value<int> --Specializes--> Value<T>
```

Observed normalized edge:

```text
Value --Instantiates--> Value
```

The source model contains both the primary `ClassTemplate` and specialized `Struct`, so extraction is not wholly missing. The relationship classification does not conform to the supplied verification contract. The fixture remains intentionally failing and the runner preserves the actual database, normalized model, validator report, and reproducibility metadata.

### Repository baseline test regression outside this sub-project

After a concurrent canonical-parent change in the core implementation, the pre-existing `codeinsight_tests` target reports:

```text
conflicting canonical parents for symbol demo::header_local::value
```

The new framework unit test passes. This core regression is recorded rather than silently changed by the conformance project.
