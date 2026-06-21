#!/usr/bin/env bash
#
# Builds a Linux AppImage for the unofficial Nuvio Desktop Linux fork.
#
# This wraps the jpackage "app image" produced by Compose's `createDistributable`
# (app + bundled JRE + the libmpv player bridge .so inside the app jar) into a
# portable AppImage. libmpv itself is NOT bundled — the app resolves the host's
# libmpv.so.2 at runtime, so the target machine must have mpv/libmpv installed
# (see CHANGELOG / README). appimagetool is downloaded on first run and cached
# under build/ (which is gitignored).
#
# Usage (normally invoked by the `packageLinuxAppImage` Gradle task):
#   scripts/build-appimage.sh <appImageDir> <outputDir> <version> <iconPng>
#
#   appImageDir  jpackage app-image dir, e.g. composeApp/build/compose/binaries/main/app/Nuvio
#   outputDir    where the .AppImage is written
#   version      display version (e.g. 0.1.8-alpha); the artifact is named
#                Nuvio-<version>-linux-<arch>.AppImage
#   iconPng      path to the app icon PNG

set -euo pipefail

APP_NAME="Nuvio"
APP_IMAGE_DIR="${1:?app-image dir required}"
OUTPUT_DIR="${2:?output dir required}"
VERSION="${3:?version required}"
ICON_PNG="${4:?icon png required}"

ARCH="$(uname -m)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Pinned SHA-256 of the upstream "continuous" appimagetool we build against.
# When upstream legitimately updates, review the new binary, then re-run with
# NUVIO_APPIMAGETOOL_SHA256=<sha> (or install appimagetool yourself and export
# $APPIMAGETOOL to skip the download entirely).
APPIMAGETOOL_SHA256_x86_64="a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0"

verify_sha256() {
    local file="$1" expected="$2" actual
    actual="$(sha256sum "$file" | awk '{print $1}')"
    [[ "$actual" == "$expected" ]] && return 0
    echo "build-appimage: appimagetool SHA-256 mismatch (expected $expected, got $actual)." >&2
    return 1
}

if [[ ! -d "$APP_IMAGE_DIR/bin" ]]; then
    echo "build-appimage: '$APP_IMAGE_DIR' is not a jpackage app image (no bin/). Run createDistributable first." >&2
    exit 1
fi

# Normalize to absolute paths: appimagetool runs mksquashfs from its own CWD,
# so a relative output path would resolve against the wrong directory.
APP_IMAGE_DIR="$(cd "$APP_IMAGE_DIR" && pwd)"
mkdir -p "$OUTPUT_DIR"; OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
ICON_PNG="$(cd "$(dirname "$ICON_PNG")" && pwd)/$(basename "$ICON_PNG")"

# --- resolve appimagetool ---------------------------------------------------
# Prefer an explicit ($APPIMAGETOOL) or system (PATH) appimagetool so most
# machines download nothing. Otherwise fetch the pinned upstream build over
# HTTPS and verify its SHA-256 before ever executing it.
if [[ -z "${APPIMAGETOOL:-}" ]] && command -v appimagetool >/dev/null 2>&1; then
    APPIMAGETOOL="$(command -v appimagetool)"
fi

if [[ -z "${APPIMAGETOOL:-}" ]]; then
    expected="${NUVIO_APPIMAGETOOL_SHA256:-}"
    if [[ -z "$expected" ]]; then
        case "$ARCH" in
            x86_64) expected="$APPIMAGETOOL_SHA256_x86_64" ;;
            *) echo "build-appimage: no pinned appimagetool hash for arch '$ARCH'. Install appimagetool and export \$APPIMAGETOOL, or set \$NUVIO_APPIMAGETOOL_SHA256." >&2; exit 1 ;;
        esac
    fi
    cache_dir="$REPO_ROOT/build/appimagetool"
    APPIMAGETOOL="$cache_dir/appimagetool-$ARCH.AppImage"
    if [[ ! -x "$APPIMAGETOOL" ]] || ! verify_sha256 "$APPIMAGETOOL" "$expected"; then
        mkdir -p "$cache_dir"
        url="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
        echo "build-appimage: downloading appimagetool ($ARCH)…" >&2
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL "$url" -o "$APPIMAGETOOL"
        else
            wget -qO "$APPIMAGETOOL" "$url"
        fi
        if ! verify_sha256 "$APPIMAGETOOL" "$expected"; then
            rm -f "$APPIMAGETOOL"
            echo "build-appimage: refusing to run unverified appimagetool. If this is a legitimate upstream update, review it and re-run with NUVIO_APPIMAGETOOL_SHA256=<sha>." >&2
            exit 1
        fi
        chmod +x "$APPIMAGETOOL"
    fi
fi

# --- assemble AppDir --------------------------------------------------------
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
APPDIR="$WORK_DIR/$APP_NAME.AppDir"

mkdir -p "$APPDIR/usr"
# jpackage image: bin/<App>, lib/{app,runtime}. Place it under usr/ wholesale.
cp -a "$APP_IMAGE_DIR/." "$APPDIR/usr/"

launcher="$APPDIR/usr/bin/$APP_NAME"
if [[ ! -x "$launcher" ]]; then
    echo "build-appimage: launcher '$launcher' missing or not executable." >&2
    exit 1
fi

# Icon: AppImage wants <icon-name>.png at the AppDir root, matching desktop Icon=.
icon_name="nuvio"
cp "$ICON_PNG" "$APPDIR/$icon_name.png"
install -Dm644 "$ICON_PNG" \
    "$APPDIR/usr/share/icons/hicolor/256x256/apps/$icon_name.png"

# Desktop entry (root + the data dir appimagetool also expects).
desktop_file="$APPDIR/$APP_NAME.desktop"
cat > "$desktop_file" <<EOF
[Desktop Entry]
Type=Application
Name=$APP_NAME
Comment=Unofficial Linux build of Nuvio Desktop
Exec=$APP_NAME
Icon=$icon_name
Categories=AudioVideo;Video;Player;
Terminal=false
EOF
install -Dm644 "$desktop_file" \
    "$APPDIR/usr/share/applications/$APP_NAME.desktop"

# AppRun: launch the jpackage binary, forwarding args. AppImages set $APPDIR.
cat > "$APPDIR/AppRun" <<EOF
#!/usr/bin/env bash
HERE="\$(dirname "\$(readlink -f "\${0}")")"
exec "\$HERE/usr/bin/$APP_NAME" "\$@"
EOF
chmod +x "$APPDIR/AppRun"

# --- pack -------------------------------------------------------------------
mkdir -p "$OUTPUT_DIR"
out="$OUTPUT_DIR/$APP_NAME-$VERSION-linux-$ARCH.AppImage"
echo "build-appimage: packing $out" >&2
# --appimage-extract-and-run avoids needing FUSE on the build host.
ARCH="$ARCH" "$APPIMAGETOOL" --appimage-extract-and-run "$APPDIR" "$out"
echo "build-appimage: done -> $out" >&2
