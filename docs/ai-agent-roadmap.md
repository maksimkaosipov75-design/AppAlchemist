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
1. Commit the current uncommitted runtime-probe changes after re-checking behavior on at least one known-good package.
2. Move runtime probe policy into a separate helper/class instead of keeping it inside `PackageToAppImagePipeline`.
3. Add profile-specific non-GUI smoke checks:
   - Electron: launcher/resources/sandbox helper checks
   - Python: interpreter and script path checks
   - Java: bundled JRE / `java -jar` readiness checks
4. Replace generic `--help` probing with profile-aware probe commands and stricter parsing of failure patterns.
5. Add structured probe results instead of plain log strings.

## Mid-Term Steps
1. Add persistent package-hash based conversion cache metadata, not only cached AppImage file lookup.
2. Add tarball subtype classification:
   - portable binary tarball
   - installer tarball
   - source tarball
   - self-contained app bundle
3. Introduce rules-driven compatibility fixes from a data file, not hardcoded branches.
4. Separate package extraction, classification, repair, validation, and packaging into distinct components.
5. Build a regression corpus of real packages and store expected outcomes.

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
