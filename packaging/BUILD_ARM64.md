# Building AppAlchemist for ARM64

This guide explains how to build AppAlchemist AppImage for ARM64 architecture (e.g., Asahi Linux on Apple M1/M2 Macs).

## Prerequisites

On your ARM64 system (Asahi Linux), ensure you have:

```bash
# Install build dependencies
sudo pacman -S base-devel cmake qt6-base qt6-tools wget
# Or on Debian/Ubuntu:
# sudo apt-get install build-essential cmake qt6-base-dev qt6-base-dev-tools wget
```

## Building

1. Clone or copy the AppAlchemist repository to your ARM64 system

2. Run the ARM64 build script:

```bash
cd /path/to/deb-to-appimage
./packaging/build-appimage-arm64.sh
```

3. The output will be in `releases/AppAlchemist-ARM64.AppImage`

## Verification

After building, verify the architecture:

```bash
file releases/AppAlchemist-ARM64.AppImage
# Should show: AppImage for ARM64/aarch64

# Extract and check the binary inside:
./releases/AppAlchemist-ARM64.AppImage --appimage-extract
file squashfs-root/usr/bin/appalchemist
# Should show: ELF 64-bit LSB pie executable, ARM aarch64
```

## Notes

- The build script automatically downloads `appimagetool-aarch64.AppImage` if not present
- The script uses a separate build directory (`build-arm64`) to avoid conflicts with x86_64 builds
- The output file is named `AppAlchemist-ARM64.AppImage` for easy identification








