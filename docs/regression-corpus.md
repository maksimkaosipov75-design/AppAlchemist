# Regression Corpus

`docs/regression-corpus.json` is the canonical manifest for conversion regression coverage.

It serves two purposes:
- keep a stable list of package cases with expected classification and planning outcomes
- separate always-available local smoke cases from optional external packages that should be gathered over time

Current policy:
- `enabled=true` means the case is expected to be runnable from this repository or the local test environment
- `optional=true` means the manifest records the target coverage even if the artifact is not checked into the repo yet
- every case should eventually describe:
  - input format
  - expected application profile
  - expected tarball subtype when applicable
  - expected conversion plan mode
  - whether AppImage creation should succeed or fail cleanly

The first seeded case is:
- `local-self-hosted-tarball`
  - source: `packaging/arch/appalchemist-1.0.0.tar.gz`
  - expected role: portable tarball smoke test for classification, probe flow, and packaging

When adding a new real package:
1. Add a new case to `docs/regression-corpus.json`.
2. Mark it `enabled=true` only when the artifact path is stable in the local environment or repository.
3. Record the expected classification and plan outcome before changing conversion logic.
4. Update the expected result only when behavior intentionally changes.
