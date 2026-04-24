#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
ARCH_DIR="$SCRIPT_DIR"

cd "$PROJECT_DIR"

echo "=== Building AppAlchemist Arch Linux Package ==="

# Clean previous builds
rm -rf "$ARCH_DIR/src" "$ARCH_DIR/pkg" "$ARCH_DIR/appalchemist-*.tar.gz"

# Create source tarball
VERSION="$(grep -E '^project\(appalchemist VERSION' CMakeLists.txt | sed -E 's/.*VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
mkdir -p "$BUILD_DIR/appalchemist-$VERSION"
cp -r src include ui CMakeLists.txt README.md "$BUILD_DIR/appalchemist-$VERSION/"
cp -r packaging "$BUILD_DIR/appalchemist-$VERSION/"
if [ -f "assets/icons/appalchemist.png" ]; then
    mkdir -p "$BUILD_DIR/appalchemist-$VERSION/assets/icons"
    cp assets/icons/appalchemist.png "$BUILD_DIR/appalchemist-$VERSION/assets/icons/"
fi
cd "$BUILD_DIR"
tar czf "$ARCH_DIR/appalchemist-$VERSION.tar.gz" "appalchemist-$VERSION"

# Build package
cd "$ARCH_DIR"
makepkg -si

echo ""
echo "=== Arch Linux Package Built Successfully ==="













