#!/bin/bash
# Install the Ubuntu/Debian system packages required by the Linux desktop builds.
# Python tooling is deliberately not installed here; scripts/bootstrap-linux.sh
# puts CMake, Ninja, and aqtinstall into the repository-local uv environment.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
snow_die() { printf '%s\n' "$*" >&2; exit 1; }

if [[ "${1:-}" == --help ]]; then
    echo 'Usage: install-linux-system-deps.sh'
    echo 'Installs the apt packages needed to configure and build the Linux desktop apps.'
    exit 0
fi

[[ "$(uname -s)" == Linux ]] || snow_die 'This script requires Linux.'
[[ -r /etc/os-release ]] || snow_die 'Cannot identify the distribution (/etc/os-release is missing).'
# shellcheck disable=SC1091
. /etc/os-release
case " ${ID:-} ${ID_LIKE:-} " in
    *" ubuntu "*|*" debian "*) ;;
    *) snow_die "This script supports Ubuntu/Debian derivatives only; found ID=${ID:-unknown}." ;;
esac
command -v apt-get >/dev/null || snow_die 'apt-get was not found.'

packages=(
    # Compilers, CMake-driven port builds, and archive tools used by vcpkg.
    # clang/libclang-dev provide the bindgen dependency for the Rust crates;
    # clang-format is required by the generated icon pack check.
    build-essential clang libclang-dev clang-format
    pkg-config autoconf automake libtool bison flex
    patch file git curl ca-certificates tar xz-utils unzip zip bzip2
    nasm yasm
    # OpenGL, X11, xcb, and input libraries required to compile and run Qt.
    libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev
    libxkbcommon-dev libxkbcommon-x11-dev
    libx11-dev libx11-xcb-dev libxext-dev libxfixes-dev libxi-dev
    libxtst-dev libxrender-dev libsm-dev libice-dev
    libxcb1-dev libxcb-cursor-dev libxcb-cursor0
    libxcb-icccm4-dev libxcb-image0-dev libxcb-keysyms1-dev
    libxcb-randr0-dev libxcb-render-util0-dev libxcb-shape0-dev
    libxcb-xkb-dev libxcb-xinerama0-dev libxcb-xfixes0-dev
    libxcb-sync-dev libxcb-glx0-dev
    libdbus-1-dev libfontconfig1-dev libfreetype-dev
    libssl-dev libvulkan1 libvulkan-dev
)

if [[ "$(id -u)" -eq 0 ]]; then
    apt=(apt-get)
else
    command -v sudo >/dev/null || snow_die 'sudo is required to install system packages.'
    apt=(sudo apt-get)
fi

"${apt[@]}" update
"${apt[@]}" install -y "${packages[@]}"

for tool in gcc g++ pkg-config nasm clang-format; do
    command -v "$tool" >/dev/null || snow_die "Package installation did not provide $tool."
done

printf 'System packages for the Linux build are installed.\n'
printf 'Next: bash scripts/bootstrap-linux.sh\n'
