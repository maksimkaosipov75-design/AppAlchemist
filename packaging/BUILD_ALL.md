# Building All AppAlchemist Packages

## Requirements

### For DEB Package:
- `debhelper` (>= 13)
- `cmake` (>= 3.15)
- `qt6-base-dev`
- `dpkg-buildpackage`

### For RPM Package:
- `rpm-build`
- `cmake` (>= 3.15)
- `qt6-qtbase-devel`
- `rpmbuild`

### For Arch Package:
- `base-devel`
- `cmake` (>= 3.15)
- `qt6-base`
- `makepkg`

## Step 1: Save the Icon

See `SAVE_ICON.md` for instructions on saving the application icon.

## Step 2: Build Packages

### Debian/Ubuntu (.deb)
```bash
cd /path/to/deb-to-appimage
./packaging/build-deb.sh
```

Result: `build/appalchemist_1.0.0-1_amd64.deb`

### Fedora/RHEL/CentOS (.rpm)
```bash
cd /path/to/deb-to-appimage
./packaging/build-rpm.sh
```

Result: `build/rpmbuild/RPMS/x86_64/appalchemist-1.0.0-1.x86_64.rpm`

### Arch Linux (PKGBUILD)
```bash
cd /path/to/deb-to-appimage/packaging/arch
./build-arch.sh
```

Or manually:
```bash
cd /path/to/deb-to-appimage/packaging/arch
makepkg -si
```

Result: `appalchemist-1.0.0-1-x86_64.pkg.tar.zst`

## Installing Packages

### DEB
```bash
sudo dpkg -i build/appalchemist_1.0.0-1_amd64.deb
sudo apt-get install -f  # Install dependencies if needed
```

### RPM
```bash
sudo rpm -ivh build/rpmbuild/RPMS/x86_64/appalchemist-1.0.0-1.x86_64.rpm
```

### Arch
```bash
sudo pacman -U appalchemist-1.0.0-1-x86_64.pkg.tar.zst
```

## Verification

After installation, the application should be available:
- In the application menu as "AppAlchemist"
- From the command line: `appalchemist`
