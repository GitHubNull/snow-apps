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
