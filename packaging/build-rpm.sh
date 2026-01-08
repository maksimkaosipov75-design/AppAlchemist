#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
PACKAGE_DIR="$PROJECT_DIR/packaging"

cd "$PROJECT_DIR"

echo "=== Building AppAlchemist RPM Package ==="

# Clean previous builds
rm -rf "$BUILD_DIR/rpmbuild" "$BUILD_DIR/appalchemist-*.tar.gz"

# Build the application
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Create source tarball
cd "$PROJECT_DIR"
VERSION="1.0.0"
mkdir -p "$BUILD_DIR/appalchemist-$VERSION"
cp -r src include ui CMakeLists.txt README.md packaging "$BUILD_DIR/appalchemist-$VERSION/"
if [ -d "assets" ]; then
    cp -r assets "$BUILD_DIR/appalchemist-$VERSION/"
fi
cd "$BUILD_DIR"
tar czf "appalchemist-$VERSION.tar.gz" "appalchemist-$VERSION"

# Setup RPM build environment
mkdir -p "$BUILD_DIR/rpmbuild"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
cp "appalchemist-$VERSION.tar.gz" "$BUILD_DIR/rpmbuild/SOURCES/"
cp "$PACKAGE_DIR/appalchemist.spec" "$BUILD_DIR/rpmbuild/SPECS/"

# Copy appimagetool if available
if [ -f "$PROJECT_DIR/thirdparty/appimagetool" ]; then
    mkdir -p "$BUILD_DIR/appalchemist-$VERSION/usr/lib/appalchemist"
    cp "$PROJECT_DIR/thirdparty/appimagetool" "$BUILD_DIR/appalchemist-$VERSION/usr/lib/appalchemist/appimagetool"
    chmod +x "$BUILD_DIR/appalchemist-$VERSION/usr/lib/appalchemist/appimagetool"
    cd "$BUILD_DIR"
    tar czf "appalchemist-$VERSION.tar.gz" "appalchemist-$VERSION"
    cp "appalchemist-$VERSION.tar.gz" "$BUILD_DIR/rpmbuild/SOURCES/"
fi

# Build RPM
cd "$BUILD_DIR/rpmbuild"
rpmbuild --define "_topdir $(pwd)" -ba SPECS/appalchemist.spec

echo ""
echo "=== RPM Package Built Successfully ==="
echo "Package location: $BUILD_DIR/rpmbuild/RPMS/x86_64/appalchemist-${VERSION}-1.x86_64.rpm"

