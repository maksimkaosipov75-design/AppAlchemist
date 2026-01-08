# How to Save the Application Icon

## Instructions

1. Open the icon image (that you provided)
2. Save it as a PNG file named `appalchemist.png`
3. Size should be 256x256 pixels or larger (square image)
4. Copy the file to the project directory:

```bash
cp /path/to/your/icon.png /path/to/deb-to-appimage/assets/icons/appalchemist.png
```

Or use any graphics editor to save the image to this directory.

## Verification

After saving the icon, verify that the file exists:

```bash
ls -lh /path/to/deb-to-appimage/assets/icons/appalchemist.png
```

## Automatic Installation

The icon will be automatically included in all packages during build:
- **DEB package**: Icon installed to `/usr/share/pixmaps/` and `/usr/share/icons/hicolor/`
- **RPM package**: Icon installed to `/usr/share/pixmaps/` and `/usr/share/icons/hicolor/`
- **Arch package**: Icon installed to `/usr/share/pixmaps/` and `/usr/share/icons/hicolor/`

If the icon is not found, packages will still build successfully, but the application will use a default icon or no icon.
