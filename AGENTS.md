# Repository Guidelines

## Project Structure & Module Organization
`src/` and `include/` contain the shared C++20 conversion backend and CLI-facing logic. `frontend/gtk/` holds the GTK4/libadwaita frontend entrypoint and styling, while `ui/` contains legacy Qt `.ui` assets that still inform older code paths. Packaging assets and release scripts live in `packaging/`, static icons in `assets/`, and design notes in `docs/`. Treat `build/`, `build-arm64/`, `releases/`, and `squashfs-root/` as generated output, not source.

## Build, Test, and Development Commands
Configure and build locally with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Run the app from the build tree with `./build/appalchemist`. Package builds are driven by repository scripts: `./packaging/build-appimage.sh`, `./packaging/build-appimage-arm64.sh`, `./packaging/build-deb.sh`, and `./packaging/build-rpm.sh`. Read `packaging/README.md` and `packaging/BUILD_ALL.md` before changing release logic.

## Coding Style & Naming Conventions
Follow the existing C++ style: 4-space indentation, opening braces on the same line, and one class per matching header/source pair such as `include/appdirbuilder.h` and `src/appdirbuilder.cpp`. Use `PascalCase` for classes, `camelCase` for methods and local variables, and lowercase filenames with underscores only where already established, such as `dependency_resolver.cpp`. Prefer Qt types and logging (`QString`, `QStringList`, `qDebug()`, `qWarning()`) in backend code.

## Testing Guidelines
There is no `ctest` or unit-test suite wired into `CMakeLists.txt` yet. Contributors should at minimum rebuild successfully, exercise CLI conversion (`./build/appalchemist --convert path/to/pkg.deb --no-launch`), and note the package type tested. Use `test-krita.sh` as a reference for manual conversion smoke tests when working on bundling or dependency resolution.

## Commit & Pull Request Guidelines
Recent commits use short imperative subjects like `Fix Java package AppImage startup` and `Improve package conversion reliability`. Keep commit titles focused, under roughly 72 characters, and scoped to one change. PRs should describe user-visible behavior, list verification commands, link the issue when applicable, and include screenshots only for frontend or packaging UX changes.

## Packaging & Release Notes
The only checked-in GitHub Actions workflow is `.github/workflows/build-arm64.yml`, which builds ARM64 AppImages on tag or release events. If you change packaging inputs, update the matching script and any impacted docs in `README.md` or `packaging/`.
