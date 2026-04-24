# Supported Packages

This page tracks package formats and real-world conversion expectations.

## Format Support

| Format | Status | Notes |
|---|---:|---|
| `.deb` | Supported | Best tested path |
| `.rpm` | Supported | Depends on upstream package layout |
| `.tar.gz` | Supported | Requires a usable Linux app structure |
| `.tar.xz` | Supported | Requires a usable Linux app structure |
| `.zip` | Supported | Requires a usable Linux app structure |

## Useful Test Matrix

| Package Type | What To Check |
|---|---|
| GTK/Qt desktop app | Launcher, icon, shared libraries, startup from terminal |
| Electron app | `/opt` layout, sandbox flags, desktop metadata |
| Java app | Bundled or system JRE assumptions |
| Python app | Interpreter path, site packages, startup script |
| Archive app bundle | Main executable detection and icon detection |

## Reporting Results

When adding a package to the regression corpus, include:

- package name and version
- source URL when public
- input format
- distribution used for conversion
- AppAlchemist version
- whether the generated AppImage starts
- notes about launcher, icon, and dependencies

See also:

- [regression-corpus.md](regression-corpus.md)
- [regression-corpus.json](regression-corpus.json)
