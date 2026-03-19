#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build-arm64"
PACKAGE_DIR="$PROJECT_DIR/packaging"
OUTPUT_DIR="$PROJECT_DIR/releases"

cd "$PROJECT_DIR"

# Force ARM64 architecture
ARCH_NAME="aarch64"

echo "=== Building AppAlchemist AppImage for ARM64 (aarch64) ==="
echo "Note: This script should be run on an ARM64 system (e.g., Asahi Linux on M1 Mac)"
echo "For cross-compilation from x86_64, you need Qt6 cross-compilation toolchain"

# Clean previous builds
rm -rf "$BUILD_DIR/appalchemist.AppDir" "$OUTPUT_DIR/AppAlchemist-ARM64.AppImage"

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

# Download appimagetool for ARM64 if not available
if [ ! -f "$PROJECT_DIR/thirdparty/appimagetool-aarch64.AppImage" ]; then
    echo "Downloading appimagetool for ARM64..."
    mkdir -p "$PROJECT_DIR/thirdparty"
    cd "$PROJECT_DIR/thirdparty"
    wget -q --show-progress "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-aarch64.AppImage" -O appimagetool-aarch64.AppImage
    chmod +x appimagetool-aarch64.AppImage
    cd "$PROJECT_DIR"
fi

# Copy appimagetool for ARM64
if [ -f "$PROJECT_DIR/thirdparty/appimagetool-aarch64.AppImage" ]; then
    echo "Copying bundled appimagetool for ARM64..."
    cp "$PROJECT_DIR/thirdparty/appimagetool-aarch64.AppImage" "$APPDIR/usr/lib/appalchemist/appimagetool"
    chmod +x "$APPDIR/usr/lib/appalchemist/appimagetool"
fi

# Copy desktop file
cp "$PACKAGE_DIR/appalchemist.desktop" "$APPDIR/usr/share/applications/"

# Copy desktop file handlers (for .deb and .rpm file associations)
cp "$PACKAGE_DIR/appalchemist-deb-handler.desktop" "$APPDIR/usr/share/applications/"
cp "$PACKAGE_DIR/appalchemist-rpm-handler.desktop" "$APPDIR/usr/share/applications/"

# Copy icon if available
if [ -f "$PROJECT_DIR/assets/icons/appalchemist.png" ]; then
    cp "$PROJECT_DIR/assets/icons/appalchemist.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/appalchemist.png"
    cp "$PROJECT_DIR/assets/icons/appalchemist.png" "$APPDIR/appalchemist.png"
fi

# Copy MIME type files
cp "$PACKAGE_DIR/mime/deb-package.xml" "$APPDIR/usr/share/mime/packages/"
cp "$PACKAGE_DIR/mime/rpm-package.xml" "$APPDIR/usr/share/mime/packages/"

# Create .desktop file in AppDir root (required by appimagetool and linuxdeploy)
cp "$APPDIR/usr/share/applications/appalchemist.desktop" "$APPDIR/"

# Download linuxdeploy and Qt plugin for ARM64 if not available
THIRDPARTY_DIR="$PROJECT_DIR/thirdparty"
mkdir -p "$THIRDPARTY_DIR"

LINUXDEPLOY=""
LINUXDEPLOY_PLUGIN_QT=""

# ARM64 specific filenames
LINUXDEPLOY_FILE="linuxdeploy-aarch64.AppImage"
LINUXDEPLOY_PLUGIN_QT_FILE="linuxdeploy-plugin-qt-aarch64.AppImage"

# Download linuxdeploy if not present
if [ ! -f "$THIRDPARTY_DIR/$LINUXDEPLOY_FILE" ]; then
    echo "Downloading linuxdeploy for ARM64..."
    cd "$THIRDPARTY_DIR"
    wget -q --show-progress "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/$LINUXDEPLOY_FILE" -O "$LINUXDEPLOY_FILE"
    chmod +x "$LINUXDEPLOY_FILE"
    cd "$PROJECT_DIR"
fi

# Download Qt plugin if not present
if [ ! -f "$THIRDPARTY_DIR/$LINUXDEPLOY_PLUGIN_QT_FILE" ]; then
    echo "Downloading linuxdeploy Qt plugin for ARM64..."
    cd "$THIRDPARTY_DIR"
    wget -q --show-progress "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/$LINUXDEPLOY_PLUGIN_QT_FILE" -O "$LINUXDEPLOY_PLUGIN_QT_FILE"
    chmod +x "$LINUXDEPLOY_PLUGIN_QT_FILE"
    cd "$PROJECT_DIR"
fi

LINUXDEPLOY="$THIRDPARTY_DIR/$LINUXDEPLOY_FILE"
LINUXDEPLOY_PLUGIN_QT="$THIRDPARTY_DIR/$LINUXDEPLOY_PLUGIN_QT_FILE"

# Bundle Qt6 dependencies using linuxdeploy
QT_BUNDLED=false
if [ -f "$LINUXDEPLOY" ] && [ -f "$LINUXDEPLOY_PLUGIN_QT" ]; then
    echo "Bundling Qt6 dependencies with linuxdeploy..."
    
    # Export plugin path for linuxdeploy
    export LINUXDEPLOY_PLUGIN_QT="$LINUXDEPLOY_PLUGIN_QT"
    
    # Run linuxdeploy to bundle dependencies
    # Ignore errors from strip (they're non-critical)
    "$LINUXDEPLOY" \
        --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/appalchemist" \
        --desktop-file "$APPDIR/appalchemist.desktop" \
        --icon-file "$APPDIR/appalchemist.png" \
        --plugin qt 2>&1 | grep -v "ERROR: Strip call failed" || true
    
    # Check if Qt6 libraries were actually bundled (even if linuxdeploy reported errors)
    if [ -f "$APPDIR/usr/lib/libQt6Core.so.6" ] && \
       [ -f "$APPDIR/usr/lib/libQt6Widgets.so.6" ] && \
       [ -f "$APPDIR/usr/lib/libQt6Network.so.6" ]; then
        QT_BUNDLED=true
        echo "✓ Qt6 libraries successfully bundled"
    else
        echo "WARNING: Qt6 libraries not found after linuxdeploy"
    fi
else
    echo "WARNING: linuxdeploy or Qt plugin not found!"
fi

# If Qt libraries weren't bundled, try manual fallback (for critical libraries only)
if [ "$QT_BUNDLED" = false ]; then
    echo "Attempting manual Qt6 library bundling..."
    
    # Find Qt6 installation
    QT6_LIB_DIR=""
    for dir in /usr/lib /usr/lib64; do
        if [ -f "$dir/libQt6Core.so.6" ]; then
            QT6_LIB_DIR="$dir"
            break
        fi
    done
    
    if [ -n "$QT6_LIB_DIR" ]; then
        echo "Found Qt6 at: $QT6_LIB_DIR"
        mkdir -p "$APPDIR/usr/lib"
        
        # Copy essential Qt6 libraries
        for lib in libQt6Core.so.6 libQt6Widgets.so.6 libQt6Network.so.6 libQt6Gui.so.6 libQt6DBus.so.6; do
            if [ -f "$QT6_LIB_DIR/$lib" ]; then
                cp "$QT6_LIB_DIR/$lib" "$APPDIR/usr/lib/" 2>/dev/null && echo "  Copied $lib" || true
            fi
        done
        
        # Copy Qt6 plugins if available
        if [ -d "$QT6_LIB_DIR/qt6/plugins" ]; then
            mkdir -p "$APPDIR/usr/lib/qt6"
            cp -r "$QT6_LIB_DIR/qt6/plugins" "$APPDIR/usr/lib/qt6/" 2>/dev/null && echo "  Copied Qt6 plugins" || true
        fi
        
        # Verify bundling
        if [ -f "$APPDIR/usr/lib/libQt6Core.so.6" ]; then
            QT_BUNDLED=true
            echo "✓ Qt6 libraries manually bundled"
        fi
    else
        echo "ERROR: Qt6 not found on system. Cannot bundle dependencies."
        echo "The AppImage will require Qt6 to be installed on the target system."
    fi
fi

# Create/update AppRun script with Qt environment variables and desktop file registration
cat > "$APPDIR/AppRun" << 'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"

# Export APPIMAGE path - this helps desktop handlers find the AppImage
# Try to find the actual AppImage file path
APPIMAGE_PATH=""
if [ -n "${APPIMAGE}" ] && [ -f "${APPIMAGE}" ]; then
    APPIMAGE_PATH="${APPIMAGE}"
else
    # Try to find AppImage in common locations
    SCRIPT_DIR="$(dirname "$(readlink -f "${0}")")"
    # If we're inside an extracted AppImage, go up to find the actual AppImage
    if [ -d "${SCRIPT_DIR}/../squashfs-root" ] || [ -f "${SCRIPT_DIR}/../.AppImage" ]; then
        APPIMAGE_PATH="$(find "$(dirname "${SCRIPT_DIR}")" -maxdepth 1 -name "appalchemist-*.AppImage" -type f 2>/dev/null | head -1)"
    fi
    # Try common download/application directories
    if [ -z "${APPIMAGE_PATH}" ]; then
        for dir in "${HOME}/Downloads" "${HOME}/Загрузки" "${HOME}/Desktop" "${HOME}/Рабочий стол" "${HOME}/AppImages" "${HOME}/Applications" "${HOME}/.local/bin"; do
            if [ -d "${dir}" ]; then
                APPIMAGE_PATH="$(find "${dir}" -maxdepth 1 -name "appalchemist-*.AppImage" -type f 2>/dev/null | head -1)"
                [ -n "${APPIMAGE_PATH}" ] && break
            fi
        done
    fi
    # Last resort: search in home directory (max 2 levels)
    if [ -z "${APPIMAGE_PATH}" ]; then
        APPIMAGE_PATH="$(find "${HOME}" -maxdepth 2 -name "appalchemist-*.AppImage" -type f 2>/dev/null | head -1)"
    fi
fi

# Export APPIMAGE if we found it
if [ -n "${APPIMAGE_PATH}" ] && [ -f "${APPIMAGE_PATH}" ]; then
    export APPIMAGE="${APPIMAGE_PATH}"
fi

export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${HERE}/usr/lib/aarch64-linux-gnu:${LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="${HERE}/usr/lib/qt6/plugins:${HERE}/usr/plugins:${QT_PLUGIN_PATH}"
export QML2_IMPORT_PATH="${HERE}/usr/lib/qml:${QML2_IMPORT_PATH}"

# Register desktop files and MIME types on first run
if [ -d "${HERE}/usr/share/applications" ]; then
    # Register desktop files (for file associations)
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "${HOME}/.local/share/applications" 2>/dev/null || true
    fi
    
    # Copy desktop handlers to user's applications directory for file associations
    # Update handlers with current APPIMAGE path if found
    if [ ! -f "${HOME}/.local/share/applications/appalchemist-deb-handler.desktop" ] || \
       [ "${HERE}/usr/share/applications/appalchemist-deb-handler.desktop" -nt "${HOME}/.local/share/applications/appalchemist-deb-handler.desktop" ]; then
        mkdir -p "${HOME}/.local/share/applications"
        cp "${HERE}/usr/share/applications/appalchemist-deb-handler.desktop" "${HOME}/.local/share/applications/" 2>/dev/null || true
        cp "${HERE}/usr/share/applications/appalchemist-rpm-handler.desktop" "${HOME}/.local/share/applications/" 2>/dev/null || true
        if command -v update-desktop-database >/dev/null 2>&1; then
            update-desktop-database "${HOME}/.local/share/applications" 2>/dev/null || true
        fi
    fi
fi

# Register MIME types
if [ -d "${HERE}/usr/share/mime/packages" ] && command -v update-mime-database >/dev/null 2>&1; then
    update-mime-database "${HOME}/.local/share/mime" 2>/dev/null || true
fi

exec "${HERE}/usr/bin/appalchemist" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# Build AppImage
mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"

# Use bundled appimagetool for ARM64
APPIMAGETOOL=""
if [ -f "$APPDIR/usr/lib/appalchemist/appimagetool" ]; then
    APPIMAGETOOL="$APPDIR/usr/lib/appalchemist/appimagetool"
elif [ -f "$PROJECT_DIR/thirdparty/appimagetool-aarch64.AppImage" ]; then
    APPIMAGETOOL="$PROJECT_DIR/thirdparty/appimagetool-aarch64.AppImage"
else
    echo "ERROR: appimagetool for ARM64 not found!"
    echo "Please download appimagetool-aarch64.AppImage from:"
    echo "https://github.com/AppImage/AppImageKit/releases"
    echo "And place it in thirdparty/appimagetool-aarch64.AppImage"
    exit 1
fi

echo "Using appimagetool: $APPIMAGETOOL"
ARCH=aarch64 "$APPIMAGETOOL" -n "$APPDIR" "$OUTPUT_DIR/AppAlchemist-ARM64.AppImage"

chmod +x "$OUTPUT_DIR/AppAlchemist-ARM64.AppImage"

echo ""
echo "=== AppImage Built Successfully ==="
echo "AppImage location: $OUTPUT_DIR/AppAlchemist-ARM64.AppImage"
echo "File size: $(du -h "$OUTPUT_DIR/AppAlchemist-ARM64.AppImage" | cut -f1)"

