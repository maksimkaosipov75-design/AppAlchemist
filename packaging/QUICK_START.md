# Quick Start - Building AppAlchemist Packages

## 1. Save the Icon

Save the provided icon image as:
```
assets/icons/appalchemist.png
```

Recommended size: 256x256 pixels, PNG format.

## 2. Build Packages

### DEB (Debian/Ubuntu)
```bash
./packaging/build-deb.sh
```
Result: `build/appalchemist_1.5.0-1_amd64.deb`

### RPM (Fedora/RHEL)
```bash
./packaging/build-rpm.sh
```
Result: `build/rpmbuild/RPMS/x86_64/appalchemist-1.5.0-1.x86_64.rpm`

### Arch Linux
```bash
cd packaging/arch
./build-arch.sh
```
Result: `appalchemist-1.5.0-1-x86_64.pkg.tar.zst`

## 3. Install the Package

### DEB
```bash
sudo dpkg -i build/appalchemist_1.5.0-1_amd64.deb
```

### RPM
```bash
sudo rpm -ivh build/rpmbuild/RPMS/x86_64/appalchemist-1.5.0-1.x86_64.rpm
```

### Arch
```bash
sudo pacman -U appalchemist-1.5.0-1-x86_64.pkg.tar.zst
```

## Notes

- Icon is optional - packages will build without it
- All scripts will automatically include the icon if it's located at `assets/icons/appalchemist.png`
- Desktop file will be created automatically for all packages
