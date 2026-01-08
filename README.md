# AppAlchemist

**Convert .deb and .rpm packages to AppImage format automatically**

AppAlchemist is a powerful tool that automatically converts Debian (.deb) and RPM (.rpm) packages into portable AppImage format. It handles dependency resolution, library bundling, and creates ready-to-run AppImages that work across different Linux distributions.

## Features

- **Automatic Conversion**: Convert .deb and .rpm packages to AppImage with a single command
- **Dependency Resolution**: Automatically detects and bundles required libraries
- **Double-Click Support**: Double-click on .deb or .rpm files to convert and launch
- **Self-Contained**: Includes appimagetool - no external dependencies required
- **Cross-Distribution**: Works on any Linux distribution that supports Qt6

## Installation

### AppImage (Recommended)

1. Download the latest `appalchemist-x86_64.AppImage` from the [Releases](https://github.com/appalchemist/appalchemist/releases) page
2. Make it executable:
   ```bash
   chmod +x appalchemist-x86_64.AppImage
   ```
3. Run it:
   ```bash
   ./appalchemist-x86_64.AppImage
   ```

### Debian/Ubuntu (.deb)

```bash
sudo dpkg -i appalchemist_1.0.0-1_amd64.deb
sudo apt-get install -f  # Install dependencies if needed
```

### Fedora/RHEL/CentOS (.rpm)

```bash
sudo rpm -i appalchemist-1.0.0-1.x86_64.rpm
```

### From Source

#### Requirements

- CMake 3.15 or higher
- Qt6 (Core, Widgets, Network)
- C++20 compatible compiler
- Development tools (make, g++)

#### Build Instructions

```bash
git clone https://github.com/appalchemist/appalchemist.git
cd appalchemist
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

## Usage

### GUI Mode

Simply run the application:

```bash
appalchemist
```

Or double-click the AppImage file.

### CLI Mode (Automatic Conversion)

Convert a package and automatically launch the resulting AppImage:

```bash
appalchemist --convert package.deb
appalchemist --convert package.rpm
```

Specify output directory:

```bash
appalchemist --convert package.deb --output ~/AppImages
```

Convert without auto-launching:

```bash
appalchemist --convert package.deb --no-launch
```

### Double-Click Conversion

After installation, you can double-click on .deb or .rpm files to automatically convert them to AppImage format. The converted AppImage will be saved to `~/AppImages/` and launched automatically.

## How It Works

1. **Package Extraction**: Extracts the package contents
2. **Metadata Analysis**: Identifies executables, icons, and desktop files
3. **Dependency Detection**: Analyzes required libraries
4. **Library Bundling**: Copies necessary libraries into the AppDir
5. **AppDir Creation**: Builds the AppImage directory structure
6. **AppImage Building**: Uses appimagetool to create the final AppImage

## AppImageTool Integration

AppAlchemist includes appimagetool internally. If appimagetool is not found in the system, AppAlchemist will:

1. First check for a bundled copy in `usr/lib/appalchemist/appimagetool`
2. Check common system locations (`/usr/bin/appimagetool`, etc.)
3. Automatically download appimagetool from GitHub if needed
4. Cache it for future use

No manual installation of appimagetool is required!

## Supported Package Types

- **.deb** (Debian/Ubuntu packages)
- **.rpm** (Fedora/RHEL/CentOS packages)

## Requirements

- Linux (x86_64)
- Qt6 runtime libraries (included in AppImage)
- Standard Linux utilities (tar, ar, rpm2cpio, etc.)

## Building Packages

### Build AppImage

```bash
cd packaging
./build-appimage.sh
```

Output: `releases/appalchemist-x86_64.AppImage`

### Build .deb Package

```bash
cd packaging
./build-deb.sh
```

Output: `build/appalchemist_1.0.0-1_amd64.deb`

### Build .rpm Package

```bash
cd packaging
./build-rpm.sh
```

Output: `build/rpmbuild/RPMS/x86_64/appalchemist-1.0.0-1.x86_64.rpm`

## Troubleshooting

### AppImage Not Launching

- Ensure the AppImage has executable permissions: `chmod +x appalchemist-x86_64.AppImage`
- Check that your system supports AppImage format

### Conversion Fails

- Verify the package file is not corrupted
- Check that you have sufficient disk space
- Ensure required system utilities are installed (tar, ar, rpm2cpio)

### AppImageTool Not Found

AppAlchemist will automatically download appimagetool if not found. If download fails:

1. Check your internet connection
2. Manually download from [AppImageKit Releases](https://github.com/AppImage/AppImageKit/releases)
3. Place it in `thirdparty/appimagetool` in the project directory

## License

MIT License - see [LICENSE](LICENSE) file for details.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## Support

For issues, questions, or feature requests, please open an issue on [GitHub](https://github.com/appalchemist/appalchemist/issues).

## Acknowledgments

- [AppImageKit](https://github.com/AppImage/AppImageKit) for the AppImage format
- Qt Project for the Qt framework
