#!/bin/bash

set -e

PACKAGE="/var/cache/pacman/pkg/krita-5.2.15-1.1-x86_64_v3.pkg.tar.zst"
OUTPUT_DIR="/tmp/krita-test"
APPIMAGE="/home/mozze0/deb-to-appimage/releases/appalchemist-1.1.0-x86_64.AppImage"

echo "=== Testing Krita Conversion ==="
echo "Package: $PACKAGE"
echo "Output: $OUTPUT_DIR"

# Clean previous test
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# Convert krita
echo ""
echo "=== Converting Krita ==="
"$APPIMAGE" --cli \
    --package "$PACKAGE" \
    --output "$OUTPUT_DIR/krita.AppImage" \
    --resolve-deps \
    --sudo-password "$(echo -n 'test' | base64)" 2>&1 | tee "$OUTPUT_DIR/conversion.log"

# Check if AppImage was created
if [ ! -f "$OUTPUT_DIR/krita.AppImage" ]; then
    echo "ERROR: AppImage was not created!"
    exit 1
fi

echo ""
echo "=== Extracting AppImage ==="
rm -rf /tmp/krita-extract
cd /tmp
"$OUTPUT_DIR/krita.AppImage" --appimage-extract 2>/dev/null

echo ""
echo "=== Checking libraries ==="
export LD_LIBRARY_PATH="/tmp/squashfs-root/usr/lib:$LD_LIBRARY_PATH"
ldd /tmp/squashfs-root/usr/bin/krita 2>&1 | grep "not found" || echo "All libraries found!"

echo ""
echo "=== Checking for gsl ==="
if [ -f "/tmp/squashfs-root/usr/lib/libgsl.so.28" ] || [ -f "/tmp/squashfs-root/usr/lib/libgsl.so.28.0.0" ]; then
    echo "✓ gsl library found"
    ls -lh /tmp/squashfs-root/usr/lib/libgsl.so* 2>/dev/null || true
else
    echo "✗ gsl library NOT found"
fi

echo ""
echo "=== Test complete ==="

