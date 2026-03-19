# AI Agent Roadmap

## Goal
Preserve the current "double-click package -> short wait -> usable app on Arch/CachyOS" workflow while increasing conversion speed, stability, and coverage for `.deb`, `.rpm`, and supported tarballs.

## Current Baseline
- Safety checkpoint commit: `36a1290` (`Create safety checkpoint before conversion refactor`)
- Rewrite checkpoint commit: `3a0e3d5` (`Add staged conversion planning pipeline`)
- Current work after `3a0e3d5` is not fully committed yet. It adds an early `AppRun` probe in:
  - `include/packagetoappimagepipeline.h`
  - `src/packagetoappimagepipeline.cpp`

## What Already Exists
- `PackageProfile` and `ConversionPlan` split packages into:
  - `fast-path-preferred`
  - `repair-fallback`
  - `legacy-fallback`
- `PackageToAppImagePipeline` now has separate execution branches for fast, repair, and fallback paths.
- `fast path` currently targets simple native desktop/CLI apps.
- `repair path` currently targets Electron, Java, Python, and Chrome-style packages.

## Immediate Next Steps
1. Done in commit `e7e090c`: runtime-probe changes were re-checked on `packaging/arch/appalchemist-1.0.0.tar.gz` and committed.
2. Done: move runtime probe policy into a separate helper/class instead of keeping it inside `PackageToAppImagePipeline`.
3. Done: add profile-specific non-GUI smoke checks:
   - Electron: launcher/resources/sandbox helper checks
   - Python: interpreter and script path checks
   - Java: bundled JRE / `java -jar` readiness checks
4. Done: replace generic `--help` probing with profile-aware probe commands and stricter parsing of failure patterns.
5. Done: add structured probe results instead of plain log strings.

## Current Status
- Runtime probe policy now lives in `include/runtime_probe.h` and `src/runtime_probe.cpp`.
- `PackageToAppImagePipeline` only coordinates and logs structured probe results.
- Probe behavior is now profile-aware:
  - Electron and Chrome-style packages use `--version` plus resource/sandbox checks
  - Python packages verify interpreter and script resolution before probing
  - Java packages verify JAR plus bundled/system Java readiness before probing
- A local smoke test already succeeded for `packaging/arch/appalchemist-1.0.0.tar.gz`, producing `/tmp/appalchemist-roadmap-smoke/appalchemist-1.AppImage`.
- Conversion cache metadata is now written and read via `CacheManager` using package-hash keyed JSON records, with legacy file/mtime lookup kept as a fallback path.
- Tarballs are now classified into explicit subtypes in `PackageProfile`, improving planning and logging for portable bundles, installer archives, source archives, and self-contained app bundles.
- Initial rules-driven compatibility fixes now load from `assets/compatibility_rules.json` via a dedicated compatibility rule engine instead of being fully hardcoded in AppRun generation.
- `PackageToAppImagePipeline` now delegates extraction, inspection/classification, and packaging to dedicated backend components instead of owning those responsibilities inline.

## Mid-Term Steps
1. Implemented in code: add persistent package-hash based conversion cache metadata, not only cached AppImage file lookup.
2. Implemented in code: add tarball subtype classification:
   - portable binary tarball
   - installer tarball
   - source tarball
   - self-contained app bundle
3. Implemented initial version in code: introduce rules-driven compatibility fixes from a data file, not hardcoded branches.
4. Implemented initial component split: separate package extraction, classification/inspection, and packaging into distinct components while keeping repair orchestration in the main pipeline.
5. Implemented initial corpus manifest: build a regression corpus of real packages and store expected outcomes.

## Guardrails
- Do not remove the legacy fallback path until replacement coverage is proven.
- Prefer additive stages over behavioral rewrites.
- After every meaningful change, run:

```bash
cmake --build build -j$(nproc)
```

- Before committing risky changes, verify at least one known working package still converts.
- Do not commit generated directories like `build-arm64/` or `build.cache-backup-*`.

## Suggested Verification Matrix
- Native Qt/GTK desktop app
- Native CLI package
- Electron app
- Python app
- Java app
- One tarball that is already portable
- One package that should intentionally fall back or fail cleanly
