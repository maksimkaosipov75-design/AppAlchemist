# Setting up the Application Icon

## Step 1: Prepare the Icon

1. Save the provided icon image as `appalchemist.png`
2. Recommended size: 256x256 pixels or larger (square)
3. Format: PNG with transparency support

## Step 2: Place the Icon

Copy the icon file to:
```
assets/icons/appalchemist.png
```

## Step 3: Verify

The icon will be automatically included in all package builds:
- **DEB package**: Icon installed to `/usr/share/pixmaps/` and `/usr/share/icons/hicolor/`
- **RPM package**: Icon installed to `/usr/share/pixmaps/` and `/usr/share/icons/hicolor/`
- **Arch package**: Icon installed to `/usr/share/pixmaps/` and `/usr/share/icons/hicolor/`

## Note

If the icon file is not found, the packages will still build successfully, but the application will use a default icon or no icon.














