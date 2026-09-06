# Testing Philosophy

The verification project is a black-box consumer of the published canonical model. Compiler-backed CodeInsight code owns extraction, identity, merge, lifecycle, and persistence; this project owns controlled inputs, integrity checks, semantic expectations, normalization, comparison, and useful failure evidence.

Every fixture first runs the product validator. Fixture-specific assertions never run against a structurally invalid model. Expectations describe semantic facts rather than SQLite IDs, row order, timestamps, temporary paths, scheduling, private class names, or log text.

Positive and relevant negative assertions belong together. A passing test must show both that required facts exist and that invalid merges or relationships do not. Broken or uncertain source must be represented as partial, failed, dependent, ambiguous, or unresolved rather than upgraded into confidence.
