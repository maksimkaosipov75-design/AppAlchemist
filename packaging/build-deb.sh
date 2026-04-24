#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
PACKAGE_DIR="$PROJECT_DIR/packaging"

cd "$PROJECT_DIR"

echo "=== Building AppAlchemist DEB Package ==="

# Clean previous builds
rm -rf "$BUILD_DIR/debian" "$BUILD_DIR/appalchemist_*" "$BUILD_DIR/appalchemist-*"

# Build the application
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Create source tarball
cd "$PROJECT_DIR"
VERSION="$(grep -E '^project\(appalchemist VERSION' CMakeLists.txt | sed -E 's/.*VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
mkdir -p "$BUILD_DIR/appalchemist-$VERSION"
cp -r src include ui CMakeLists.txt README.md packaging "$BUILD_DIR/appalchemist-$VERSION/"
if [ -d "assets" ]; then
    cp -r assets "$BUILD_DIR/appalchemist-$VERSION/"
fi
cd "$BUILD_DIR"
tar czf "appalchemist_$VERSION.orig.tar.gz" "appalchemist-$VERSION"

# Copy packaging files
cp -r "$PACKAGE_DIR/debian" "appalchemist-$VERSION/"

# Copy appimagetool if available
cd "appalchemist-$VERSION"
if [ -f "$PROJECT_DIR/thirdparty/appimagetool" ]; then
    mkdir -p "usr/lib/appalchemist"
    cp "$PROJECT_DIR/thirdparty/appimagetool" "usr/lib/appalchemist/appimagetool"
    chmod +x "usr/lib/appalchemist/appimagetool"
fi

# Build the package
dpkg-buildpackage -us -uc -b

echo ""
echo "=== DEB Package Built Successfully ==="
echo "Package location: $BUILD_DIR/appalchemist_${VERSION}-1_amd64.deb"
