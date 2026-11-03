# Linux build support (Phase 0)

Phase 0 is a verification milestone: Linux presets build the shared libraries and
Snow Image Viewer. Snow Shot itself is not built on Linux yet, because it needs a
Rust screen-capture backend and a C++ X11/Wayland platform layer that do not exist
in this repository.

Phase 0 builds:

- `snow_image` (all full-profile codecs)
- `ant_design_qt` (icons and widgets)
- `snow_draw_engine_qt_canvas` and its Rust `snow_draw_engine_c` core
- `snow_image_viewer` and `snow_image_edit_worker`

## Prerequisites

Ubuntu 24.04 or a derivative such as Linux Mint 22, GCC 13, Rust 1.97.1 or newer,
and the X11/OpenGL development packages Qt and vcpkg need to compile and run:

```bash
bash scripts/install-linux-system-deps.sh
```

The script installs the compiler, X11/xcb, OpenGL, and archive tools listed in its
`packages` array with `apt-get` through `sudo`. `bash scripts/bootstrap-linux.sh
--install-system-deps` runs the same script. Python tooling is deliberately not
part of this step.

Python tooling is isolated in the repository-local uv virtual environment at
`.tools/venv`. Do not install `cmake`, `ninja`, or `aqtinstall` into the global
Python environment; the bootstrap script never does.

## Bootstrap

```bash
bash scripts/bootstrap-linux.sh
```

The script:

1. creates `.tools/venv` with `uv venv` and installs `cmake` (4.2 or newer),
   `ninja`, and `aqtinstall` with `uv pip install --python .tools/venv/bin/python`;
2. installs Qt 6.12.0 for `linux_gcc_64` into `.tools/Qt` with the venv's `aqt`
   (the Qt 6.12 base package already contains Qt Svg, Qt Tools/LinguistTools, and
   Qt Translations);
3. checks the active Rust toolchain and adds the `rustfmt`/`clippy` components
   when `rustup` manages it;
4. clones the pinned vcpkg baseline into `.tools/vcpkg`, bootstraps the vcpkg
   tool, and installs the manifest features `snow-shot;full-codecs;image-viewer`
   into `.tools/vcpkg/installed/linux-dynamic` with the `x64-linux` triplet and
   the repository overlay ports.

The first vcpkg run builds OpenCV, libjxl, libheif, and x265, so expect several
minutes to an hour and a few GB of disk.

Both `aqt` and `vcpkg` follow the standard `https_proxy`/`http_proxy` environment
variables, which helps when the Qt mirror or GitHub downloads are slow:

```bash
https_proxy=http://127.0.0.1:7890 http_proxy=http://127.0.0.1:7890 \
no_proxy=localhost,127.0.0.1 bash scripts/bootstrap-linux.sh
```

Use `--skip-qt`, `--skip-dependency-install`, or `--install-system-deps` to skip
or extend individual steps.

## Build and verify

`cmake` must be the venv copy, because the system CMake is usually older than the
required 4.2. The presets put `.tools/venv/bin` on `PATH` so CMake finds Ninja.

```bash
.tools/venv/bin/cmake --preset linux-debug
.tools/venv/bin/cmake --build --preset build-linux-debug
QT_QPA_PLATFORM=offscreen ./build/linux-debug/snow_image_viewer/snow_image_viewer --version
```

The `linux-debug` preset sets `Qt6_DIR` to
`.tools/Qt/6.12.0/gcc_64/lib/cmake/Qt6`; keep it in sync with
`scripts/qt-toolchain.json`, or override it with `-DQt6_DIR=...`.

Phase 0 intentionally keeps strict warnings off (`SNOW_APPS_STRICT_COMPILE=OFF`)
and does not build the test matrix. Both return in the next phase.

## Known limitations

- Snow Shot is disabled (`SNOW_APPS_BUILD_SNOW_SHOT=OFF`); Linux screen capture,
  recording, OCR integration, and FFmpeg linkage are not implemented.
- Snow Image Viewer's Windows integrations (File Explorer, printing, sharing) use
  their existing non-Windows stubs.
- Wayland behavior is unvalidated; the first smoke run uses the offscreen
  platform plugin.
- Packaging, installers, static Qt, and ARM64 are out of scope.
