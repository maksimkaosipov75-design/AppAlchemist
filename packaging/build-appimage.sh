#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
PACKAGE_DIR="$PROJECT_DIR/packaging"
OUTPUT_DIR="$PROJECT_DIR/releases"

cd "$PROJECT_DIR"

echo "=== Building AppAlchemist AppImage ==="

# Clean previous builds
rm -rf "$BUILD_DIR/appalchemist.AppDir" "$OUTPUT_DIR"

# Build the application
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
make -j$(nproc)
make install DESTDIR="$BUILD_DIR/appalchemist.AppDir"

# Create AppDir structure
APPDIR="$BUILD_DIR/appalchemist.AppDir"
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/lib/appalchemist"
mkdir -p "$APPDIR/usr/share/applications"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
mkdir -p "$APPDIR/usr/share/mime/packages"

# Copy appimagetool if available
if [ -f "$PROJECT_DIR/thirdparty/appimagetool" ]; then
    echo "Copying bundled appimagetool..."
    cp "$PROJECT_DIR/thirdparty/appimagetool" "$APPDIR/usr/lib/appalchemist/appimagetool"
    chmod +x "$APPDIR/usr/lib/appalchemist/appimagetool"
fi

# Copy desktop file
cp "$PACKAGE_DIR/appalchemist.desktop" "$APPDIR/usr/share/applications/"

# Copy icon if available
if [ -f "$PROJECT_DIR/assets/icons/appalchemist.png" ]; then
    cp "$PROJECT_DIR/assets/icons/appalchemist.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/appalchemist.png"
    cp "$PROJECT_DIR/assets/icons/appalchemist.png" "$APPDIR/appalchemist.png"
fi

# Copy MIME type files
cp "$PACKAGE_DIR/mime/deb-package.xml" "$APPDIR/usr/share/mime/packages/"
cp "$PACKAGE_DIR/mime/rpm-package.xml" "$APPDIR/usr/share/mime/packages/"

# Create AppRun script
cat > "$APPDIR/AppRun" << 'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"
exec "${HERE}/usr/bin/appalchemist" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# Create .desktop file in AppDir root (required by appimagetool)
cp "$APPDIR/usr/share/applications/appalchemist.desktop" "$APPDIR/"

# Build AppImage
mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"

# Use bundled appimagetool if available, otherwise try system one
APPIMAGETOOL=""
if [ -f "$APPDIR/usr/lib/appalchemist/appimagetool" ]; then
    APPIMAGETOOL="$APPDIR/usr/lib/appalchemist/appimagetool"
elif [ -f "$PROJECT_DIR/thirdparty/appimagetool" ]; then
    APPIMAGETOOL="$PROJECT_DIR/thirdparty/appimagetool"
elif command -v appimagetool >/dev/null 2>&1; then
    APPIMAGETOOL="appimagetool"
else
    echo "ERROR: appimagetool not found!"
    echo "Please download appimagetool from:"
    echo "https://github.com/AppImage/AppImageKit/releases"
    echo "And place it in thirdparty/appimagetool"
    exit 1
fi

echo "Using appimagetool: $APPIMAGETOOL"
ARCH=x86_64 "$APPIMAGETOOL" -n "$APPDIR" "$OUTPUT_DIR/appalchemist-x86_64.AppImage"

chmod +x "$OUTPUT_DIR/appalchemist-x86_64.AppImage"

echo ""
echo "=== AppImage Built Successfully ==="
echo "AppImage location: $OUTPUT_DIR/appalchemist-x86_64.AppImage"

