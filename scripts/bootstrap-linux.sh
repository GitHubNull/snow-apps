#!/bin/bash
# Phase 0 Linux bootstrap: tools, Qt, vcpkg dependencies.
# Python tooling lives only in the repository-local uv virtual environment.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
snow_die() { printf '%s\n' "$*" >&2; exit 1; }

if [[ "${1:-}" == --help ]]; then
    echo 'Usage: bootstrap-linux.sh [--skip-qt] [--skip-dependency-install] [--install-system-deps]'
    echo 'Installs CMake/Ninja/aqtinstall into .tools/venv with uv, Qt into .tools/Qt,'
    echo 'and the vcpkg manifest dependencies into .tools/vcpkg/installed/linux-dynamic.'
    exit 0
fi

[[ "$(uname -s)" == Linux ]] || snow_die 'This entry point requires Linux.'
for tool in git curl gcc g++ pkg-config uv rustc cargo; do
    command -v "$tool" >/dev/null || snow_die "Missing $tool. Install the prerequisites listed in docs-linux-build.md."
done

skip_qt=0
skip_dependencies=0
install_system_deps=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-qt) skip_qt=1 ;;
        --skip-dependency-install) skip_dependencies=1 ;;
        --install-system-deps) install_system_deps=1 ;;
        *) snow_die "Unknown argument: $1" ;;
    esac
    shift
done

venv="$repo_root/.tools/venv"
mkdir -p "$repo_root/.tools"
if [[ ! -x "$venv/bin/python" ]]; then
    uv venv --python 3.12 "$venv"
fi
# CMake-driven dependency builds (including fcitx5-qt) resolve Ninja and the
# compiler from PATH; keep the repository-local tools first.
export PATH="$venv/bin:$PATH"
uv pip install --python "$venv/bin/python" "cmake>=4.2,<5" ninja aqtinstall "clang-format==23.1.3" "libclang==18.1.1"
cmake_version="$("$venv/bin/cmake" --version | head -n 1)"
[[ "$cmake_version" =~ ^cmake[[:space:]]version[[:space:]]([0-9]+)\.([0-9]+) ]] ||
    snow_die "Unable to determine the venv CMake version: ${cmake_version:-unknown}"
if (( BASH_REMATCH[1] < 4 || (BASH_REMATCH[1] == 4 && BASH_REMATCH[2] < 2) )); then
    snow_die "CMake 4.2 or newer is required; found: ${cmake_version}"
fi

rust_minimum="$("$venv/bin/python" -c 'import json,sys; print(json.load(open(sys.argv[1]))["rustMinimum"])' \
    "$repo_root/scripts/build-toolchain.json")"
rust_version="$(rustc --version)" || snow_die 'Unable to determine the active Rust version.'
if ! "$venv/bin/python" - "$rust_version" "$rust_minimum" <<'PY'
import re
import sys

match = re.match(r"rustc (\d+)\.(\d+)\.(\d+)", sys.argv[1])
minimum = tuple(map(int, sys.argv[2].split(".")))
sys.exit(0 if match and tuple(map(int, match.groups())) >= minimum else 1)
PY
then
    snow_die "Rust $rust_minimum or newer is required; found: $rust_version"
fi

if command -v rustup >/dev/null; then
    installed_components="$(rustup component list --installed)" || snow_die 'Unable to list installed Rust components.'
    missing_components=()
    for component in rustfmt clippy; do
        if ! grep -Eq "^${component}-" <<< "$installed_components"; then
            missing_components+=("$component")
        fi
    done
    if [[ ${#missing_components[@]} -gt 0 ]]; then
        rustup component add "${missing_components[@]}"
    fi
fi

qt_version="$("$venv/bin/python" -c 'import json,sys; print(json.load(open(sys.argv[1]))["qtVersion"])' \
    "$repo_root/scripts/qt-toolchain.json")"
qt_arch=linux_gcc_64
qt_root="$repo_root/.tools/Qt"
qt_dir="$qt_root/$qt_version/gcc_64"
qt_cmake_dir="$qt_dir/lib/cmake/Qt6"

if [[ "$install_system_deps" == 1 ]]; then
    bash "$repo_root/scripts/install-linux-system-deps.sh"
fi

if [[ "$skip_qt" == 0 && ! -f "$qt_cmake_dir/Qt6Config.cmake" ]]; then
    "$venv/bin/aqt" install-qt linux desktop "$qt_version" "$qt_arch" \
        -O "$qt_root" --timeout 60
fi
if [[ "$skip_qt" == 0 ]]; then
    [[ -f "$qt_cmake_dir/Qt6Config.cmake" ]] ||
        snow_die "Qt $qt_version was not installed under $qt_dir."
    if ! "$qt_dir/bin/qtpaths" --qt-version >/dev/null 2>&1; then
        printf 'Warning: Qt tools failed to start. Install the system packages listed in docs-linux-build.md.\n' >&2
    fi
fi

# The distribution's Qt6 fcitx5 plugin targets the distribution Qt and is
# rejected by the bundled Qt kit, so build the input context plugin against the
# bundled kit. Fcitx5Core/Config/Utils development files and ECM are required.
fcitx5_qt_version="5.1.9"
fcitx5_qt_root="$repo_root/.tools/fcitx5-qt"
fcitx5_qt_plugin="$qt_dir/plugins/platforminputcontexts/libfcitx5platforminputcontextplugin.so"
if [[ "$skip_qt" == 0 && ! -f "$fcitx5_qt_plugin" ]]; then
    for fcitx5_pc in Fcitx5Core Fcitx5Config Fcitx5Utils; do
        pkg-config --exists "$fcitx5_pc" ||
            snow_die "Missing $fcitx5_pc development files. Run: sudo bash scripts/install-linux-system-deps.sh"
    done
    if [[ ! -d "$fcitx5_qt_root/src/.git" ]]; then
        rm -rf "$fcitx5_qt_root/src"
        mkdir -p "$fcitx5_qt_root"
        git clone --depth 1 --branch "$fcitx5_qt_version" \
            https://github.com/fcitx/fcitx5-qt.git "$fcitx5_qt_root/src"
    fi
    # Qt 6.12's `find_package(Qt6Gui ... Private)` does not create
    # Qt6::GuiPrivate for this project layout; request the private package
    # explicitly. The Wayland workaround needs a private Wayland target this
    # kit does not export, and the X11 plugin does not require it.
    if ! grep -q 'find_package(Qt6GuiPrivate' "$fcitx5_qt_root/src/qt6/CMakeLists.txt"; then
        sed -i '/find_package(Qt6Gui ${REQUIRED_QT6_VERSION} REQUIRED Private)/a find_package(Qt6GuiPrivate ${REQUIRED_QT6_VERSION} REQUIRED)' \
            "$fcitx5_qt_root/src/qt6/CMakeLists.txt"
    fi
    "$venv/bin/cmake" -S "$fcitx5_qt_root/src" -B "$fcitx5_qt_root/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$qt_dir" \
        -DCMAKE_INSTALL_PREFIX="$qt_dir" \
        -DENABLE_QT4=OFF -DENABLE_QT5=OFF -DENABLE_QT6=ON \
        -DENABLE_QT6_WAYLAND_WORKAROUND=OFF \
        -DBUILD_ONLY_PLUGIN=ON
    "$venv/bin/cmake" --build "$fcitx5_qt_root/build" -j "$(nproc)"
    "$venv/bin/cmake" --install "$fcitx5_qt_root/build"
    if [[ ! -f "$fcitx5_qt_plugin" ]]; then
        # Qt installs may redirect plugin destinations; copy the built plugin.
        fcitx5_built_plugin="$(find "$fcitx5_qt_root/build" "$qt_dir" \
            -name libfcitx5platforminputcontextplugin.so -print -quit)"
        [[ -n "$fcitx5_built_plugin" ]] ||
            snow_die "fcitx5-qt built but its Qt6 input context plugin was not found."
        mkdir -p "$(dirname "$fcitx5_qt_plugin")"
        cp -f "$fcitx5_built_plugin" "$fcitx5_qt_plugin"
    fi
fi

vcpkg_root="$repo_root/.tools/vcpkg"
vcpkg_baseline=4497409a47f19db373a410a0efb84eca4747adbf
vcpkg_installed="$vcpkg_root/installed/linux-dynamic"
if [[ ! -d "$vcpkg_root/.git" ]]; then
    [[ ! -e "$vcpkg_root" ]] || snow_die "Repository-local vcpkg must be a Git checkout: $vcpkg_root"
    git clone https://github.com/microsoft/vcpkg.git "$vcpkg_root"
fi
vcpkg_head="$(git -C "$vcpkg_root" rev-parse HEAD 2>/dev/null || true)"
if [[ "$vcpkg_head" != "$vcpkg_baseline" ]]; then
    git -C "$vcpkg_root" fetch origin "$vcpkg_baseline" --depth=1
    git -C "$vcpkg_root" checkout --detach "$vcpkg_baseline"
fi
if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
    "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
fi

if [[ "$skip_dependencies" == 0 ]]; then
    "$vcpkg_root/vcpkg" install \
        "--x-manifest-root=$repo_root" \
        "--x-install-root=$vcpkg_installed" \
        "--triplet=x64-linux" \
        --x-feature=snow-shot \
        --x-feature=full-codecs \
        --x-feature=image-viewer \
        "--overlay-ports=$repo_root/cmake/vcpkg-overlay-ports" \
        "--overlay-triplets=$repo_root/cmake/vcpkg-overlay-triplets" \
        --clean-after-build
fi

printf 'Linux build environment is ready.\n'
printf 'Repository: %s\n' "$repo_root"
printf 'CMake: %s (.tools/venv/bin/cmake)\n' "$cmake_version"
printf 'Qt6_DIR: %s\n' "$qt_cmake_dir"
printf 'vcpkg installed: %s\n' "$vcpkg_installed"
printf 'Next: .tools/venv/bin/cmake --preset linux-debug\n'
printf '      .tools/venv/bin/cmake --build --preset build-linux-debug\n'
