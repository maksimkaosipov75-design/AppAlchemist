# Contributing to AppAlchemist

Thank you for your interest in improving AppAlchemist.

## Good First Contributions

- Test package conversion with real-world `.deb` and `.rpm` files
- Test archive conversion with `.tar.gz`, `.tar.xz` and `.zip` app bundles
- Report packages that fail to convert
- Improve documentation
- Add regression test cases
- Improve launcher and icon detection

## Reporting Conversion Failures

Please include:

- Input package name and version
- Input package format
- Linux distribution
- AppAlchemist version
- Full conversion log
- Generated AppDir structure, if available
- Expected result
- Actual result

## Development Setup

Install the build dependencies for your distribution, then run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
./build/appalchemist --help
```

The project currently builds a GTK4/libadwaita frontend backed by Qt6-based conversion components.

## Pull Requests

Keep changes focused and easy to review. For conversion logic changes, include at least one package or archive used for manual testing.
