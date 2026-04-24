#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
PACKAGE_DIR="$PROJECT_DIR/packaging"
OUTPUT_DIR="$PROJECT_DIR/releases"

cd "$PROJECT_DIR"

# Detect architecture
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)
        ARCH_NAME="x86_64"
        ;;
    aarch64|arm64)
        ARCH_NAME="aarch64"
        ;;
    *)
        echo "ERROR: Unsupported architecture: $ARCH"
        exit 1
        ;;
esac

echo "=== Building AppAlchemist AppImage for $ARCH_NAME ==="

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

# The project is GTK4-only. Always package the freshly installed appalchemist
# binary from the current build instead of any stale legacy frontend artifact.
if [ ! -x "$APPDIR/usr/bin/appalchemist" ]; then
    echo "ERROR: Freshly built GTK executable not found at $APPDIR/usr/bin/appalchemist"
    exit 1
fi

# Copy appimagetool if available
if [ -f "$PROJECT_DIR/thirdparty/appimagetool" ]; then
    echo "Copying bundled appimagetool..."
    cp "$PROJECT_DIR/thirdparty/appimagetool" "$APPDIR/usr/lib/appalchemist/appimagetool"
    chmod +x "$APPDIR/usr/lib/appalchemist/appimagetool"
fi

# Copy desktop file
cp "$PACKAGE_DIR/appalchemist.desktop" "$APPDIR/usr/share/applications/"

# Copy desktop file handlers (for .deb and .rpm file associations)
cp "$PACKAGE_DIR/appalchemist-deb-handler.desktop" "$APPDIR/usr/share/applications/"
cp "$PACKAGE_DIR/appalchemist-rpm-handler.desktop" "$APPDIR/usr/share/applications/"

# Create wrapper script to find and launch AppImage
cat > "$APPDIR/usr/bin/appalchemist-wrapper" << 'EOF'
#!/bin/bash
# Wrapper script to find AppImage and launch it with --convert

# Configuration file to store AppImage path
CONFIG_FILE="${HOME}/.config/appalchemist/appimage_path"

# Try to find AppImage location
APPIMAGE_PATH=""

# Method 1: Check if APPIMAGE variable is set (when launched from AppImage)
if [ -z "${APPIMAGE_PATH}" ] && [ -n "${APPIMAGE}" ] && [ -f "${APPIMAGE}" ]; then
    APPIMAGE_PATH="${APPIMAGE}"
    # Save path for future use
    mkdir -p "$(dirname "${CONFIG_FILE}")"
    echo "${APPIMAGE_PATH}" > "${CONFIG_FILE}" 2>/dev/null || true
fi

# Method 2: Check saved path from config file
if [ -z "${APPIMAGE_PATH}" ] && [ -f "${CONFIG_FILE}" ]; then
    SAVED_PATH="$(cat "${CONFIG_FILE}" 2>/dev/null | head -1)"
    if [ -n "${SAVED_PATH}" ] && [ -f "${SAVED_PATH}" ]; then
        APPIMAGE_PATH="${SAVED_PATH}"
    fi
fi

# Method 3: Search in common locations (including subdirectories)
if [ -z "${APPIMAGE_PATH}" ]; then
    for dir in "${HOME}/Downloads" "${HOME}/Загрузки" "${HOME}/Desktop" "${HOME}/Рабочий стол" "${HOME}/AppImages" "${HOME}/Applications" "${HOME}/.local/bin" "${HOME}/deb-to-appimage/releases" "${HOME}/deb-to-appimage" "/opt" "/usr/local/bin"; do
        if [ -d "${dir}" ]; then
            # Search in directory and one level deep
            APPIMAGE_PATH="$(find "${dir}" -maxdepth 2 -name "appalchemist-*.AppImage" -type f 2>/dev/null | head -1)"
            [ -n "${APPIMAGE_PATH}" ] && break
        fi
    done
fi

# Method 4: Search in home directory (max 5 levels deep, but prioritize common locations)
if [ -z "${APPIMAGE_PATH}" ]; then
    # First try common project directories
    for pattern in "*/releases/appalchemist-*.AppImage" "*/deb-to-appimage/appalchemist-*.AppImage" "*/appalchemist-*.AppImage"; do
        APPIMAGE_PATH="$(find "${HOME}" -maxdepth 5 -path "${pattern}" -type f 2>/dev/null | head -1)"
        [ -n "${APPIMAGE_PATH}" ] && break
    done
    # If still not found, do broader search
    if [ -z "${APPIMAGE_PATH}" ]; then
        APPIMAGE_PATH="$(find "${HOME}" -maxdepth 5 -name "appalchemist-*.AppImage" -type f 2>/dev/null | head -1)"
    fi
fi

# Method 5: Check if appalchemist is in PATH (system installation)
if [ -z "${APPIMAGE_PATH}" ] && command -v appalchemist >/dev/null 2>&1; then
    SYSTEM_APPALCHEMIST="$(command -v appalchemist)"
    if [ "${SYSTEM_APPALCHEMIST}" != "${0}" ]; then
        exec "${SYSTEM_APPALCHEMIST}" "$@"
    fi
    exit $?
fi

# Save found path for future use
if [ -n "${APPIMAGE_PATH}" ] && [ -f "${APPIMAGE_PATH}" ]; then
    mkdir -p "$(dirname "${CONFIG_FILE}")"
    echo "${APPIMAGE_PATH}" > "${CONFIG_FILE}" 2>/dev/null || true
    ERROR_LOG="$(mktemp)"
    if "${APPIMAGE_PATH}" "$@" 2>"${ERROR_LOG}"; then
        rm -f "${ERROR_LOG}"
        exit 0
    fi

    if grep -qiE "Cannot mount AppImage|failed to open /dev/fuse|FUSE setup" "${ERROR_LOG}"; then
        rm -f "${ERROR_LOG}"
        exec env APPIMAGE_EXTRACT_AND_RUN=1 "${APPIMAGE_PATH}" "$@"
    fi

    cat "${ERROR_LOG}" >&2
    rm -f "${ERROR_LOG}"
    exit 1
fi

# Error: AppImage not found
notify-send "AppAlchemist" "AppAlchemist not found. Please download AppImage from GitHub releases." 2>/dev/null || \
    echo "AppAlchemist not found. Please download AppImage from GitHub releases." >&2
exit 1
EOF
chmod +x "$APPDIR/usr/bin/appalchemist-wrapper"

# Copy icon if available
if [ -f "$PROJECT_DIR/assets/icons/appalchemist.png" ]; then
    cp "$PROJECT_DIR/assets/icons/appalchemist.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/appalchemist.png"
    # Keep a root icon with a linuxdeploy-compatible resolution.
    # Some sources ship a 1024x1024 icon, but linuxdeploy accepts up to 512x512.
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$PROJECT_DIR/assets/icons/appalchemist.png" "$APPDIR/appalchemist.png" <<'PY'
import shutil
import sys

src = sys.argv[1]
dst = sys.argv[2]

try:
    from PIL import Image
    img = Image.open(src).convert("RGBA")
    if img.width > 512 or img.height > 512:
        img = img.resize((256, 256), Image.Resampling.LANCZOS)
    img.save(dst)
except Exception:
    # Fallback: keep original icon if Pillow is unavailable.
    shutil.copy2(src, dst)
PY
    else
        cp "$PROJECT_DIR/assets/icons/appalchemist.png" "$APPDIR/appalchemist.png"
    fi
fi

# Copy MIME type files
cp "$PACKAGE_DIR/mime/deb-package.xml" "$APPDIR/usr/share/mime/packages/"
cp "$PACKAGE_DIR/mime/rpm-package.xml" "$APPDIR/usr/share/mime/packages/"

# Create .desktop file in AppDir root (required by appimagetool and linuxdeploy)
cp "$APPDIR/usr/share/applications/appalchemist.desktop" "$APPDIR/"

# Download linuxdeploy and Qt plugin if not available
THIRDPARTY_DIR="$PROJECT_DIR/thirdparty"
mkdir -p "$THIRDPARTY_DIR"

LINUXDEPLOY=""
# Determine linuxdeploy filename based on architecture
if [ "$ARCH_NAME" = "x86_64" ]; then
    LINUXDEPLOY_FILE="linuxdeploy-x86_64.AppImage"
else
    LINUXDEPLOY_FILE="linuxdeploy-${ARCH_NAME}.AppImage"
fi

# Download linuxdeploy if not present
if [ ! -f "$THIRDPARTY_DIR/$LINUXDEPLOY_FILE" ]; then
    echo "Downloading linuxdeploy..."
    cd "$THIRDPARTY_DIR"
    wget -q --show-progress "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/$LINUXDEPLOY_FILE" -O "$LINUXDEPLOY_FILE"
    chmod +x "$LINUXDEPLOY_FILE"
    cd "$PROJECT_DIR"
fi

LINUXDEPLOY="$THIRDPARTY_DIR/$LINUXDEPLOY_FILE"
# Bundle desktop dependencies using generic linuxdeploy.
DESKTOP_RUNTIME_BUNDLED=false
if [ -f "$LINUXDEPLOY" ]; then
    echo "Bundling desktop dependencies with linuxdeploy..."

    "$LINUXDEPLOY" \
        --appdir "$APPDIR" \
        --executable "$APPDIR/usr/bin/appalchemist" \
        --desktop-file "$APPDIR/appalchemist.desktop" \
        --icon-file "$APPDIR/appalchemist.png" 2>&1 | grep -v "ERROR: Strip call failed" || true

    if [ -f "$APPDIR/usr/lib/libgtk-4.so.1" ] || [ -f "$APPDIR/usr/lib/libadwaita-1.so.0" ]; then
        DESKTOP_RUNTIME_BUNDLED=true
        echo "✓ GTK/libadwaita libraries successfully bundled"
    else
        echo "WARNING: Expected GTK/libadwaita runtime libraries were not found after linuxdeploy"
    fi
else
    echo "WARNING: linuxdeploy not found!"
fi

# Bundle GTK runtime data that linuxdeploy often misses for gtk4/libadwaita apps.
# Without these, the AppImage may build fine but fail to start on user systems.
echo "Bundling GTK runtime data..."

mkdir -p "$APPDIR/usr/share/glib-2.0/schemas"
if [ -d "/usr/share/glib-2.0/schemas" ]; then
    cp -a /usr/share/glib-2.0/schemas/. "$APPDIR/usr/share/glib-2.0/schemas/" 2>/dev/null || true
fi

mkdir -p "$APPDIR/usr/lib/gio/modules"
if [ -d "/usr/lib/gio/modules" ]; then
    cp -a /usr/lib/gio/modules/. "$APPDIR/usr/lib/gio/modules/" 2>/dev/null || true
fi

if [ -d "/usr/lib/gdk-pixbuf-2.0/2.10.0" ]; then
    mkdir -p "$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0"
    cp -a /usr/lib/gdk-pixbuf-2.0/2.10.0/loaders "$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/" 2>/dev/null || true
    if command -v gdk-pixbuf-query-loaders >/dev/null 2>&1; then
        GDK_PIXBUF_MODULEDIR="$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders" \
            gdk-pixbuf-query-loaders > "$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" 2>/dev/null || true
    elif [ -f "/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" ]; then
        sed "s|/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders|$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders|g" \
            /usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache > "$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" 2>/dev/null || true
    fi
    if [ -f "$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" ]; then
        sed "s|$APPDIR|@APPDIR@|g" \
            "$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" \
            > "$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache.in" 2>/dev/null || true
    fi
fi

if [ -d "/usr/lib/gtk-4.0/4.0.0/immodules" ]; then
    mkdir -p "$APPDIR/usr/lib/gtk-4.0/4.0.0"
    cp -a /usr/lib/gtk-4.0/4.0.0/immodules "$APPDIR/usr/lib/gtk-4.0/4.0.0/" 2>/dev/null || true
    if command -v gtk4-query-immodules >/dev/null 2>&1; then
        gtk4-query-immodules "$APPDIR/usr/lib/gtk-4.0/4.0.0/immodules/"*.so \
            > "$APPDIR/usr/lib/gtk-4.0/4.0.0/immodules.cache" 2>/dev/null || true
    elif command -v gtk-query-immodules-4.0 >/dev/null 2>&1; then
        gtk-query-immodules-4.0 "$APPDIR/usr/lib/gtk-4.0/4.0.0/immodules/"*.so \
            > "$APPDIR/usr/lib/gtk-4.0/4.0.0/immodules.cache" 2>/dev/null || true
    fi
fi

# Create/update AppRun script for the GTK-first executable.
# linuxdeploy may leave AppRun as a symlink to usr/bin/appalchemist.
# Remove it first so we don't accidentally overwrite the main binary.
rm -f "$APPDIR/AppRun"
cat > "$APPDIR/AppRun" << 'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"

# Export APPIMAGE path - AppImage runtime sets this automatically
# But we need to save it for wrapper script
if [ -n "${APPIMAGE}" ] && [ -f "${APPIMAGE}" ]; then
    # Save path to config file for wrapper script
    mkdir -p "${HOME}/.config/appalchemist"
    echo "${APPIMAGE}" > "${HOME}/.config/appalchemist/appimage_path" 2>/dev/null || true
else
    # Try to find AppImage path if APPIMAGE variable is not set
    APPIMAGE_PATH=""
    # Check /proc/self/exe
    if [ -L /proc/self/exe ] || [ -f /proc/self/exe ]; then
        EXE_PATH="$(readlink -f /proc/self/exe 2>/dev/null)"
        if [ -n "${EXE_PATH}" ] && echo "${EXE_PATH}" | grep -q "\.AppImage$"; then
            APPIMAGE_PATH="${EXE_PATH}"
        fi
    fi
    # If found, export and save
    if [ -n "${APPIMAGE_PATH}" ] && [ -f "${APPIMAGE_PATH}" ]; then
        export APPIMAGE="${APPIMAGE_PATH}"
        mkdir -p "${HOME}/.config/appalchemist"
        echo "${APPIMAGE_PATH}" > "${HOME}/.config/appalchemist/appimage_path" 2>/dev/null || true
    fi
fi

export PATH="${HERE}/usr/bin:${HOME}/.local/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${HERE}/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH}"
export XDG_DATA_DIRS="${HERE}/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export GSETTINGS_SCHEMA_DIR="${HERE}/usr/share/glib-2.0/schemas:${GSETTINGS_SCHEMA_DIR:-}"
export GI_TYPELIB_PATH="${HERE}/usr/lib/girepository-1.0:${GI_TYPELIB_PATH:-}"

# Isolate the bundled GTK4 runtime from host-side GTK2/GTK3 modules and IM hooks.
# Host environment variables here often make AppImages fail before the first window.
unset GTK_PATH
unset GTK_EXE_PREFIX
unset GTK_DATA_PREFIX
unset GTK_MODULES

export GIO_MODULE_DIR="${HERE}/usr/lib/gio/modules${GIO_MODULE_DIR:+:${GIO_MODULE_DIR}}"
export GDK_PIXBUF_MODULEDIR="${HERE}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders"
export GTK_IM_MODULE_DIR="${HERE}/usr/lib/gtk-4.0/4.0.0/immodules"
export GTK_IM_MODULE="${GTK_IM_MODULE:-simple}"

if [ -f "${HERE}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache.in" ]; then
    GDK_PIXBUF_RUNTIME_CACHE="${XDG_CACHE_HOME:-${HOME}/.cache}/appalchemist/gdk-pixbuf-loaders.cache"
    mkdir -p "$(dirname "${GDK_PIXBUF_RUNTIME_CACHE}")"
    sed "s|@APPDIR@|${HERE}|g" \
        "${HERE}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache.in" > "${GDK_PIXBUF_RUNTIME_CACHE}" 2>/dev/null || true
    if [ -f "${GDK_PIXBUF_RUNTIME_CACHE}" ]; then
        export GDK_PIXBUF_MODULE_FILE="${GDK_PIXBUF_RUNTIME_CACHE}"
    fi
elif [ -f "${HERE}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" ]; then
    export GDK_PIXBUF_MODULE_FILE="${HERE}/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
fi

if [ -f "${HERE}/usr/lib/gtk-4.0/4.0.0/immodules.cache" ]; then
    export GTK_IM_MODULE_FILE="${HERE}/usr/lib/gtk-4.0/4.0.0/immodules.cache"
fi

# Register desktop files and MIME types on first run
if [ -d "${HERE}/usr/share/applications" ]; then
    # Register desktop files (for file associations)
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "${HOME}/.local/share/applications" 2>/dev/null || true
    fi
    
    # Copy desktop handlers to user's applications directory for file associations
    # Also copy wrapper script to user's bin directory
    if [ ! -f "${HOME}/.local/share/applications/appalchemist.desktop" ] || \
       [ "${HERE}/usr/share/applications/appalchemist.desktop" -nt "${HOME}/.local/share/applications/appalchemist.desktop" ] || \
       [ ! -f "${HOME}/.local/share/applications/appalchemist-deb-handler.desktop" ] || \
       [ "${HERE}/usr/share/applications/appalchemist-deb-handler.desktop" -nt "${HOME}/.local/share/applications/appalchemist-deb-handler.desktop" ]; then
        mkdir -p "${HOME}/.local/share/applications"
        mkdir -p "${HOME}/.local/bin"
        
        # Copy main launcher desktop entry
        cp "${HERE}/usr/share/applications/appalchemist.desktop" "${HOME}/.local/share/applications/" 2>/dev/null || true

        # Copy desktop handlers
        cp "${HERE}/usr/share/applications/appalchemist-deb-handler.desktop" "${HOME}/.local/share/applications/" 2>/dev/null || true
        cp "${HERE}/usr/share/applications/appalchemist-rpm-handler.desktop" "${HOME}/.local/share/applications/" 2>/dev/null || true
        
        # Copy wrapper script to user's bin (so it's in PATH)
        if [ -f "${HERE}/usr/bin/appalchemist-wrapper" ]; then
            cp "${HERE}/usr/bin/appalchemist-wrapper" "${HOME}/.local/bin/appalchemist-wrapper" 2>/dev/null || true
            chmod +x "${HOME}/.local/bin/appalchemist-wrapper" 2>/dev/null || true
        fi
        
        if command -v update-desktop-database >/dev/null 2>&1; then
            update-desktop-database "${HOME}/.local/share/applications" 2>/dev/null || true
        fi
    fi
fi

# Register MIME types
if [ -d "${HERE}/usr/share/mime/packages" ] && command -v update-mime-database >/dev/null 2>&1; then
    mkdir -p "${HOME}/.local/share/mime"
    update-mime-database "${HOME}/.local/share/mime" 2>/dev/null || true
fi

exec "${HERE}/usr/bin/appalchemist" "$@"
EOF
chmod +x "$APPDIR/AppRun"

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

# Get version from CMakeLists.txt or use default
VERSION=$(grep -E "^project\(appalchemist VERSION" "$PROJECT_DIR/CMakeLists.txt" | sed -E 's/.*VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/' || echo "1.5.0")
APPIMAGE_NAME="appalchemist-${VERSION}-${ARCH_NAME}.AppImage"
APPIMAGE_PATH="$OUTPUT_DIR/$APPIMAGE_NAME"

ARCH=$ARCH_NAME "$APPIMAGETOOL" -n "$APPDIR" "$APPIMAGE_PATH"

chmod +x "$APPIMAGE_PATH"

echo ""
echo "=== AppImage Built Successfully ==="
echo "AppImage location: $APPIMAGE_PATH"
echo "File size: $(du -h "$APPIMAGE_PATH" | cut -f1)"

# Generate a fallback launcher that does not require FUSE mount support.
# Useful in restricted environments where /dev/fuse is blocked.
FALLBACK_LAUNCHER="$OUTPUT_DIR/run-appalchemist.sh"
cat > "$FALLBACK_LAUNCHER" << EOF
#!/bin/bash
set -euo pipefail
APPIMAGE_PATH="$APPIMAGE_PATH"
if [ ! -f "\$APPIMAGE_PATH" ]; then
  echo "AppImage not found: \$APPIMAGE_PATH" >&2
  exit 1
fi
APPIMAGE_EXTRACT_AND_RUN=1 "\$APPIMAGE_PATH" "\$@"
EOF
chmod +x "$FALLBACK_LAUNCHER"
echo "Fallback launcher (no FUSE required): $FALLBACK_LAUNCHER"
