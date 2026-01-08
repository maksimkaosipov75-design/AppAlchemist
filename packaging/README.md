# AppAlchemist Packaging

This directory contains packaging files for building distribution packages.

## Icon

Place the application icon as `assets/icons/appalchemist.png` (256x256 or larger, PNG format).

## Building Packages

### Debian/Ubuntu (.deb)
```bash
./build-deb.sh
```

### Fedora/RHEL (.rpm)
```bash
./build-rpm.sh
```

### Arch Linux (PKGBUILD)
```bash
cd arch && makepkg -si
```






