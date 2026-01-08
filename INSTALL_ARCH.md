# Installing AppAlchemist on Arch Linux

## Package Built and Ready for Installation

**Location:** `packaging/arch/appalchemist-1.0.0-1-x86_64.pkg.tar.zst`

## Installation

```bash
cd /path/to/deb-to-appimage/packaging/arch
sudo pacman -U appalchemist-1.0.0-1-x86_64.pkg.tar.zst
```

## After Installation

Update the desktop file cache:

```bash
sudo update-desktop-database
```

Or simply restart your session/system.

## Verification

After installation, the application will be available:
- In the application menu as **"AppAlchemist"**
- From the command line: `appalchemist`

## Running

```bash
appalchemist
```

Or find "AppAlchemist" in your desktop environment's application menu.
