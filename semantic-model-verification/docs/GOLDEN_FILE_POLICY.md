# Golden File Policy

Ordinary test execution never creates or overwrites a golden file. A fixture may opt into a normalized snapshot at `expected/semantic.snapshot.txt`.

Update requires the explicit command `semantic-test update-golden FIXTURE`. The engineer must inspect the semantic diff and confirm that the behavior change is intentional before retaining the new file. Storage IDs, temporary paths, and row order are prohibited from goldens.

Small fixtures should prefer focused declarative assertions. Goldens are intended for larger graphs where a complete deterministic semantic review adds value.
