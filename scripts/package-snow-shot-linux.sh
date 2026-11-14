#!/bin/bash
# Builds the Linux AppImage and DEB packages from a linux-release install tree.
#
# Usage: scripts/package-snow-shot-linux.sh [--build-dir DIR] [--stage-dir DIR]
#        [--output-dir DIR] [--skip-appimage] [--skip-deb]
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build/linux-release"
stage_dir="$repo_root/artifacts/linux/install"
output_dir="$repo_root/artifacts/linux"
skip_appimage=0
skip_deb=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) build_dir="$2"; shift 2 ;;
        --stage-dir) stage_dir="$2"; shift 2 ;;
        --output-dir) output_dir="$2"; shift 2 ;;
        --skip-appimage) skip_appimage=1; shift ;;
        --skip-deb) skip_deb=1; shift ;;
        --help)
            sed -n '2,7p' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

cmake="${cmake:-$repo_root/.tools/venv/bin/cmake}"
[[ -x "$cmake" ]] || cmake=cmake
version="$(sed -n 's/^set(SNOW_SHOT_VERSION "\([0-9][0-9.]*\)").*/\1/p' "$repo_root/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || { echo "Unable to read SNOW_SHOT_VERSION" >&2; exit 1; }
mkdir -p "$output_dir"

if [[ ! -d "$build_dir" ]]; then
    echo "Release build tree not found: $build_dir" >&2
    echo "Configure and build it first: $cmake --preset linux-release && $cmake --build --preset build-linux-release" >&2
    exit 1
fi

echo "Installing the release tree into $stage_dir"
rm -rf "$stage_dir"
mkdir -p "$stage_dir"
"$cmake" --install "$build_dir" --prefix "$stage_dir" >/dev/null

desktop_file="$stage_dir/share/applications/snow-shot.desktop"
icon_file="$stage_dir/share/icons/hicolor/scalable/apps/snow-shot.svg"
[[ -x "$stage_dir/bin/snow_shot" ]] || { echo "Missing staged snow_shot binary" >&2; exit 1; }
[[ -f "$desktop_file" ]] || { echo "Missing staged desktop entry" >&2; exit 1; }
[[ -f "$icon_file" ]] || { echo "Missing staged application icon" >&2; exit 1; }

if [[ "$skip_appimage" == 0 ]]; then
    appimage_tool_dir="$repo_root/.tools/appimage"
    appimage_tool="$appimage_tool_dir/appimagetool-x86_64.AppImage"
    appimage_tool_sha256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
    mkdir -p "$appimage_tool_dir"
    if [[ ! -f "$appimage_tool" ]]; then
        echo "Downloading appimagetool 1.9.1"
        curl -fsSL -o "$appimage_tool" \
            https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
    fi
    echo "$appimage_tool_sha256  $appimage_tool" | sha256sum --check --status ||
        { echo "appimagetool checksum mismatch" >&2; exit 1; }
    chmod +x "$appimage_tool"

    appdir="$output_dir/AppDir"
    rm -rf "$appdir"
    mkdir -p "$appdir"
    cp -a "$stage_dir/." "$appdir/"
    cp "$desktop_file" "$appdir/snow-shot.desktop"
    cp "$icon_file" "$appdir/snow-shot.svg"
    ln -sf snow-shot.svg "$appdir/.DirIcon"
    cat > "$appdir/AppRun" <<'RUN'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export QT_PLUGIN_PATH="$HERE/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/bin/snow_shot" "$@"
RUN
    chmod +x "$appdir/AppRun"

    appimage="$output_dir/snow-shot-$version-linux-x64.AppImage"
    rm -f "$appimage"
    echo "Building $appimage"
    ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$appimage_tool" --no-appstream "$appdir" "$appimage"
    (cd "$output_dir" && sha256sum "$(basename "$appimage")" > "$(basename "$appimage").sha256")
fi

if [[ "$skip_deb" == 0 ]]; then
    pkgroot="$output_dir/deb-root"
    rm -rf "$pkgroot"
    mkdir -p "$pkgroot/DEBIAN" "$pkgroot/opt/snow-shot" "$pkgroot/usr/bin" \
        "$pkgroot/usr/share/applications" "$pkgroot/usr/share/icons/hicolor/scalable/apps"
    cp -a "$stage_dir/." "$pkgroot/opt/snow-shot/"
    ln -s /opt/snow-shot/bin/snow_shot "$pkgroot/usr/bin/snow-shot"
    cp "$desktop_file" "$pkgroot/usr/share/applications/snow-shot.desktop"
    cp "$icon_file" "$pkgroot/usr/share/icons/hicolor/scalable/apps/snow-shot.svg"
    cat > "$pkgroot/DEBIAN/control" <<CONTROL
Package: snow-shot
Version: $version
Architecture: amd64
Maintainer: Snow Shot <snowshot@users.noreply.github.com>
Section: graphics
Priority: optional
Depends: libc6 (>= 2.35), libstdc++6 (>= 12), libx11-6, libx11-xcb1, libxcb1, libxcb-cursor0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-render-util0, libxcb-shape0, libxcb-xkb1, libxkbcommon-x11-0, libgl1, libegl1, libpulse0, libcups2, libfontconfig1, libfreetype6, libdbus-1-3, libssl3, libvulkan1
Recommends: fcitx5-frontend-qt6 | ibus
Description: Screenshot capture, annotation, OCR and screen recording for Linux
 Snow Shot captures screenshots and live pinned windows, annotates and
 recognizes text, translates selections, and records the desktop on X11.
CONTROL
    chmod 644 "$pkgroot/DEBIAN/control"
    deb="$output_dir/snow-shot_${version}_amd64.deb"
    rm -f "$deb"
    echo "Building $deb"
    dpkg-deb --root-owner-group --build "$pkgroot" "$deb" >/dev/null
    (cd "$output_dir" && sha256sum "$(basename "$deb")" > "$(basename "$deb").sha256")
fi

echo "Linux packages are ready in $output_dir"
ls -lh "$output_dir" | awk 'NR>1 && $9 ~ /\.(AppImage|deb)$/ {print "  "$9" ("$5")"}'
