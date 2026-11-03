# Linux build support (Phases 0-1)

Phase 0 verified that the Linux presets compile the shared libraries and Snow
Image Viewer. Phase 1 makes Snow Image Viewer usable on a real X11 desktop: it
opens PNG/JPEG/WebP/GIF images, renders them with OpenGL, and resizes/exports
through the isolated `snow_image_edit_worker` process.

Snow Shot itself is still not built on Linux, because it needs a Rust
screen-capture backend and a C++ X11/Wayland platform layer that do not exist in
this repository.

Phase 1 builds:

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

The viewer requires an X11 session and OpenGL 3.3 or newer. A compositor is
recommended but not required.

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

The `--version` offscreen run is a headless smoke test only. See the desktop
smoke test below for the real runtime verification.

## Viewer tests

```bash
.tools/venv/bin/cmake --preset linux-debug-tests
.tools/venv/bin/cmake --build --preset build-linux-viewer-tests
.tools/venv/bin/ctest --test-dir build/linux-debug-tests \
    -R '^snow_image_viewer' -LE performance --output-on-failure -j4
```

`linux-debug-tests` inherits `linux-debug` and turns on `SNOW_APPS_BUILD_TESTS`.
The build preset only builds the viewer test executables and the edit worker;
tests from other projects are registered but never compiled.

`-LE performance` excludes `snow_image_viewer_edit_mode_performance`, which
asserts the Windows-only GPU-resident edit pipeline. The remaining suite covers
decoding, export reuse, worker lifecycle, notification-lifetime destruction, the
editor lifecycle, generated icon packs, and non-ASCII paths
(`snow_image_viewer_unicode_path_tests`, which runs `--unicode-path-only`).

## Desktop smoke test (X11)

Generate fixtures and open each format on the real display:

```bash
mkdir -p /tmp/snow-p1
ffmpeg -y -f lavfi -i testsrc2=size=1280x720:rate=1 -frames:v 1 /tmp/snow-p1/basic.png
ffmpeg -y -i /tmp/snow-p1/basic.png /tmp/snow-p1/basic.jpg
ffmpeg -y -i /tmp/snow-p1/basic.png -c:v libwebp /tmp/snow-p1/basic.webp
ffmpeg -y -f lavfi -i testsrc2=size=320x200:rate=5 -t 1 /tmp/snow-p1/basic.gif
cp /tmp/snow-p1/basic.png "/tmp/snow-p1/中文 图片.png"

DISPLAY=:0 ./build/linux-debug/snow_image_viewer/snow_image_viewer \
    "/tmp/snow-p1/中文 图片.png"
```

Capture the window without touching the session:

```bash
wid=$(DISPLAY=:0 xdotool search --sync --onlyvisible --name 'Snow Image Viewer' | head -1)
DISPLAY=:0 xdotool windowactivate "$wid"
DISPLAY=:0 xwd -root -silent > /tmp/snow-p1/shot.xwd
ffmpeg -y -loglevel error -i /tmp/snow-p1/shot.xwd /tmp/snow-p1/shot.png
```

Close the window (`DISPLAY=:0 xdotool windowclose "$wid"`) and restore the
previous focus with `xdotool windowactivate "$prev"`.

The in-app edit benchmark also drives the complete worker pipeline
(decode -> resize -> encode -> artifact copy). Run it with a real image and a
non-Windows-specific expectation:

```bash
DISPLAY=:0 ./build/linux-debug/snow_image_viewer/snow_image_viewer \
    --edit-performance-test \
    --edit-performance-output /tmp/snow-p1/edit-mode-performance.json \
    /tmp/snow-p1/basic.png
```

It completes all measured iterations and writes the report, but exits with
`The edit benchmark did not retain the requested pipeline.` on Linux by design;
see the known limitations.

## Linux-specific runtime fixes (Phase 1)

- Native path conversion is centralized in
  `snow_image_viewer/src/core/native_path.h`. POSIX platforms map `QString` paths
  to UTF-8 instead of `toStdU16String()`, so non-ASCII file names decode, edit,
  and export correctly.
- The edit worker's per-job memory ceiling on Linux uses `RLIMIT_AS`, which
  bounds the whole virtual address space. The job value is a working-set
  estimate, so the worker adds its current address space (from
  `/proc/self/statm`) before lowering the soft limit.
- Worker `QProcess` objects are detached from the controller before deferred
  deletion. Observers may destroy the controller from a worker notification
  (`failed`, `finished`) while Qt is still delivering that signal.
- The image context menu uses the platform wording "Open Containing Folder"
  outside Windows; the Windows label and File Explorer integration are unchanged.

## Known limitations

- Snow Shot is disabled (`SNOW_APPS_BUILD_SNOW_SHOT=OFF`); Linux screen capture,
  recording, OCR integration, and FFmpeg linkage are not implemented.
- The GPU-resident edit pipeline is Windows-only. On Linux the viewer displays
  images through QRhi/OpenGL but resizes and exports with the CPU reference path
  and the isolated worker. `snow_image_viewer_edit_mode_performance` therefore
  reports a residency failure and is excluded with `-LE performance`.
- Wayland is unvalidated; the verified desktop path is X11 with OpenGL 3.3 or
  newer.
- Snow Image Viewer's Windows integrations (File Explorer selection, printing,
  sharing) fall back to their existing non-Windows stubs.
- The viewer has no crash handler and writes no log file; diagnostics go to
  stderr.
- Packaging, installers, static Qt, and ARM64 are out of scope.
