# Linux build support (Phases 0-3)

Phase 0 verified that the Linux presets compile the shared libraries and Snow
Image Viewer. Phase 1 made Snow Image Viewer usable on a real X11 desktop: it
opens PNG/JPEG/WebP/GIF images, renders them with OpenGL, and resizes/exports
through the isolated `snow_image_edit_worker` process. Phase 2 builds Snow Shot
itself and runs the screenshot workflow on X11 with a Qt-based capture provider:
launch the main window, take a screenshot, drag a selection, edit it, and copy or
save the result. Phase 3 closes the P0/P1 gaps: reproducible bootstrap with a
pinned venv toolchain, an install tree with a desktop entry and bundled Qt
runtime, X11 global shortcuts, direct capture from the tray/MCP entry points,
cursor compositing through XFixes, hiding the app's own windows before grabs,
Wayland session detection, and multi-screen/mixed-DPI geometry tests.

The native Rust capture backends (Windows Graphics Capture, ScreenCaptureKit)
and the Cocoa/CoreGraphics platform layer remain unavailable on Linux. Snow Shot
therefore uses `QScreen::grabWindow` for still screenshots and keeps the existing
UI, editing, and export pipelines unchanged; screen recording stays disabled.

Phase 2 builds:

- `snow_image` (all full-profile codecs)
- `ant_design_qt` (icons and widgets)
- `snow_draw_engine_qt_canvas` and its Rust `snow_draw_engine_c` core
- `snow_image_viewer` and `snow_image_edit_worker`
- `snow_shot`, its Rust `snow_shot_rust_ffi` bundle, and the FFmpeg static link

## Prerequisites

Ubuntu 24.04 or a derivative such as Linux Mint 22, GCC 13, Rust 1.97.1 or newer,
and the X11/OpenGL development packages Qt and vcpkg need to compile and run:

```bash
bash scripts/install-linux-system-deps.sh
```

The script installs the compiler, LLVM/Clang (`clang`, `libclang-dev`,
`clang-format`), X11/xcb, OpenGL, and archive tools listed in its `packages`
array with `apt-get` through `sudo`. `bash scripts/bootstrap-linux.sh
--install-system-deps` runs the same script. Python tooling is deliberately not
part of this step.

Python tooling is isolated in the repository-local uv virtual environment at
`.tools/venv`. Do not install `cmake`, `ninja`, `aqtinstall`, `clang-format`, or
`libclang` into the global Python environment; the bootstrap script never does.

The desktop applications require an X11 session and OpenGL 3.3 or newer. A
compositor is recommended but not required.

## Bootstrap

```bash
bash scripts/bootstrap-linux.sh
```

The script:

1. creates `.tools/venv` with `uv venv` and installs `cmake` (4.2 or newer),
   `ninja`, `aqtinstall`, `clang-format`, and the pinned `libclang` 18.1.1 (used
   by Rust bindgen when no system libclang exists) with
   `uv pip install --python .tools/venv/bin/python`;
2. installs Qt 6.12.0 for `linux_gcc_64` into `.tools/Qt` with the venv's `aqt`
   (the Qt 6.12 base package already contains Qt Svg, Qt Tools/LinguistTools, and
   Qt Translations);
3. checks the active Rust toolchain and adds the `rustfmt`/`clippy` components
   when `rustup` manages it;
4. clones the pinned vcpkg baseline into `.tools/vcpkg`, bootstraps the vcpkg
   tool, and installs the manifest features `snow-shot;full-codecs;image-viewer`
   into `.tools/vcpkg/installed/linux-dynamic` with the `x64-linux` triplet and
   the repository overlay ports.

The `snow-shot` feature pulls a static FFmpeg 9 build (avcodec, avformat,
avfilter, swresample, swscale) for the Rust recording crates and the C++
diagnostics code. The first vcpkg run also builds OpenCV, libjxl, libheif, and
x265, so expect several minutes to an hour and a few GB of disk.

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
required 4.2. The presets put `.tools/venv/bin` on `PATH` so CMake finds Ninja
and the venv `clang-format`.

```bash
.tools/venv/bin/cmake --preset linux-debug
.tools/venv/bin/cmake --build --preset build-linux-debug
QT_QPA_PLATFORM=offscreen ./build/linux-debug/snow_image_viewer/snow_image_viewer --version
QT_QPA_PLATFORM=offscreen ./build/linux-debug/snow_shot/snow_shot --help
```

The `linux-debug` preset sets `Qt6_DIR` to
`.tools/Qt/6.12.0/gcc_64/lib/cmake/Qt6`; keep it in sync with
`scripts/qt-toolchain.json`, or override it with `-DQt6_DIR=...`.

The `--version`/`--help` offscreen runs are headless smoke tests only. See the
desktop smoke tests below for the real runtime verification.

## Tests

```bash
.tools/venv/bin/cmake --preset linux-debug-tests
.tools/venv/bin/cmake --build --preset build-linux-viewer-tests
.tools/venv/bin/ctest --test-dir build/linux-debug-tests \
    -R '^snow_image_viewer' -LE performance --output-on-failure -j4
.tools/venv/bin/ctest --test-dir build/linux-debug-tests \
    -R 'linux-capture-geometry' --output-on-failure
```

`linux-debug-tests` inherits `linux-debug` and turns on `SNOW_APPS_BUILD_TESTS`.
The build preset only builds the viewer test executables, the edit worker, and
the Linux capture geometry test; tests from other projects are registered but
never compiled.

`-LE performance` excludes `snow_image_viewer_edit_mode_performance`, which
asserts the Windows-only GPU-resident edit pipeline. The viewer suite covers
decoding, export reuse, worker lifecycle, notification-lifetime destruction, the
editor lifecycle, generated icon packs, and non-ASCII paths
(`snow_image_viewer_unicode_path_tests`, which runs `--unicode-path-only`).

`snow-shot-linux-capture-geometry-tests` covers the Linux capture provider:
display model identity (stable IDs, `physicalRect`, DPR, canvas coordinates),
virtual-desktop cropping offsets, negative-origin multi-screen unions, and
mixed-DPI physical-rect rounding (`physicalRectForGeometry`). It runs offscreen
and does not capture the real desktop. Build it with
`build-linux-snow-shot-tests`.

`snow-shot-linux-x11-logic-tests` covers the X11 shortcut/window helpers
offscreen: lock-mask combinations, Qt-to-X11 keysym mapping and unmappable
shortcut rejection, key-press classification, and the capture window shield
hiding and restoring the app's own top-level widgets (with
`WA_DontShowOnScreen` stand-ins).

The Snow Shot unit tests for the shortcut manager, settings catalog, feature
action router, system tray controller, direct-capture workflow/frame, and
global mouse stack run offscreen like their Windows/macOS counterparts; filter
them with `ctest -R 'snow-shot'`. The interactive
`snow-shot-global-shortcut-native-smoke` test claims real X11 key grabs and
must run on a live X11 display.

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

Snow Shot starts with its main window visible on Linux. Start it, enable the
floating toolbar once (Settings -> "Tray & floating toolbar" -> Floating
toolbar -> "Show toolbar"), then click its snowflake button, drag a selection
on the overlay, and use the toolbar to edit, copy, or save:

```bash
DISPLAY=:0 ./build/linux-debug/snow_shot/snow_shot
```

The floating toolbar and the global shortcuts are the most reliable capture
entries on Linux. When the desktop provides a StatusNotifier host, the tray
icon registers and the tray menu is available; on desktops without a tray or
StatusNotifier host, only the floating toolbar, the main window, and the
global shortcuts remain. The tray menu hides the commands that cannot run on
Linux (recording, OCR/translation family) instead of listing dead items.

Verify the clipboard and the saved file independently:

```bash
DISPLAY=:0 xclip -selection clipboard -t image/png -o > /tmp/snow-p1/clipboard.png
ffprobe -v error -show_entries stream=width,height -of csv=p=0 /tmp/snow-p1/clipboard.png
```

The in-app edit benchmark of the viewer also drives the complete worker pipeline
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

## Linux capture path (Phase 2)

- `ScreenshotQtCaptureProvider` enumerates `QGuiApplication::screens()`, builds
  the same `CapturedDisplayModel` geometry as the overlay preparation step
  (`ScreenshotGeometryMapper::preCaptureDisplayModel`), and grabs the X11 root
  window once through `QScreen::grabWindow(0)` on the primary screen. Each
  display is cropped from the virtual-desktop pixmap by its `physicalRect`; when
  X reports separate screens that do not match the virtual desktop, the provider
  falls back to grabbing each screen individually.
- `ScreenshotCaptureCoordinator` has a Linux branch that replaces the FFI worker
  thread with queued GUI-thread calls. It keeps the `prepared` / `layoutReady` /
  `captureFinished` signal contract and the generation-based cancellation
  semantics, so the capture workflow, overlay, and toolbar code are unchanged.
- Frame images are `QImage::Format_RGB32` with device pixel ratio 1 and sRGB
  color space, sized to the physical rectangle of each display. The cursor is
  composited into the frame through XFixes (`XFixesGetCursorImage`, sampled on
  the GUI thread right before the grab and premultiplied into the frame) when
  the "Show cursor" screenshot setting is on; the color picker's
  `cursorPatch`/`cursorPixelRect` account for the extra cursor pixels.
- Rust still builds the `snow-capture`, `snow-recording-c`, and draw-engine
  static bundles because the C++ code links them for editing and export. The
  capture provider never calls the Rust capture backend on Linux; the Rust
  recording runtime is linked but unused.
- The Linux build links the vcpkg FFmpeg archives with the pkg-config static
  closure from `cmake/SnowShotLinux.cmake`. Cargo's native link metadata is not
  propagated to CMake for static archives, so the same closure is attached to
  the Rust FFI interface targets and the `snow_shot` executable.
- Windows-only integration points are stubbed: `captureWindowId` returns
  `std::nullopt` from `snow_shot/src/platform/linux/windowcaptureexclusion.cpp`.
  Own-window exclusion is implemented on X11 instead: before a grab, the
  coordinator hides every visible top-level widget of the app
  (`capturewindowshield.cpp`) and waits for the X server to report them
  unviewable, so the app's main window, toolbar, and pinned windows never
  appear in the captured frame. The startup updater transaction, crashpad
  handler, and startup registration services are compiled only for
  Windows/macOS; on Linux the application starts with the main window visible
  unless `--autostart` is passed.
- Direct capture (tray "Capture current monitor" / "Capture focused window" and
  the MCP capture entry points) runs on Linux through
  `directcaptureqtx11.cpp`: the worker thread asks the GUI thread to perform
  the root grab and per-display crop, resolving the focused window via
  `_NET_ACTIVE_WINDOW` and `XTranslateCoordinates` for the crop rectangle.
  The backend reports `SNOW_CAPTURE_BACKEND_X11_QT`.
- The targets that embed Snow Image's bundled codec archives
  (`snow_image_shared`, `snow_shot_image_codec_backend`, and the
  `snow_shot`/`snow_image_viewer` executables) link with
  `--exclude-libs,ALL` on Linux. Without the isolation their exported
  `png_*`/zlib symbols preempt the system libpng that GTK's gdk-pixbuf uses,
  and native file dialogs abort with `munmap_chunk(): invalid pointer` while
  loading PNG icons.
- Rust bindgen uses `libclang`. When no system `clang` executable exists, the
  build picks the libclang shipped in the repository venv and adds the GCC
  include directories to `CPATH` so the FFmpeg bindings can resolve system
  headers. Installing `clang`/`libclang-dev` from the system dependency script
  keeps the regular discovery path.

## Linux global shortcuts (Phase 3)

Global shortcuts on X11 use `XGrabKey` through the shared
`snow_shot/src/platform/linux/` helpers:

- `x11connection.cpp` owns one RAII display connection and installs a
  process-wide no-op X11 error handler so `BadAccess` conflicts from other
  grabbers never reach Qt's handler.
- `x11grabkey.cpp` maps Qt keys/keypad variants to X11 keysyms, rejects
  unmappable or multi-stroke shortcuts, and grabs every lock-mask combination
  (Lock/Mod2/Mod5) so Caps Lock and layout modifiers do not mask the grab.
  A conflicting grab rolls back already-registered grabs and reports the
  conflict instead of half-registering.
- `globalshortcutbackend.cpp` feeds key events into the existing
  `GlobalShortcutBackend` contract; the registry/settings UI and the rest of
  the shortcut stack are platform-neutral and unchanged.

The shortcut backend source is excluded from the unity build
(`SKIP_UNITY_BUILD_INCLUSION`) because the X11 headers must not leak into
other translation units. The backend is only constructed when the session is
not Wayland (see below); otherwise the app keeps the unsupported-backend
fallback.

## Install and packaging (Phase 3)

`cmake --install` (or the `linux-release` preset) installs Snow Shot with
`qt_generate_deploy_app_script` bundling the Qt libraries the binary actually
links, a curated plugin whitelist, and `INSTALL_RPATH` `$ORIGIN/../lib`, into
`bin/snow_shot` + `lib/` + `share/applications/snow-shot.desktop`. The desktop
entry sets `StartupWMClass=snow_shot` so the window groups with the launcher,
and carries localized `Name`/`Comment` for zh. The install tree runs without
any system Qt and passes the `ldd`/`nm -D png_` symbol-isolation checks
described below.

## Wayland session detection (Phase 3)

`featureavailability.cpp` blocks the screenshot feature family when the
application runs on the literal `wayland` QPA platform, and both the main
window and startup warning surfaces tell the user that capture needs an X11
session (with XWayland available as a fallback session type). The offscreen
and minimal QPA platforms used by tests do not trip the gate.

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

- Screen recording and audio/video export are disabled on Linux
  (`FeatureFamily::ScreenRecording` returns false); the native capture backends
  only exist for Windows and macOS.
- OCR, translation, QR code, table, and LaTeX recognition are unavailable. The
  toolbar hides those tools because `snow_ocr_process` is not built on Linux
  (its Rust workspace depends on the uninitialized `simd-paddle-ocr-rs`
  submodule).
- Smart selection, window selection, and scrolling capture depend on native or
  Rust backends that are not built on Linux; scrolling capture is hidden and the
  remaining selection runs through the non-native overlay path. Live (continuously
  updating) pinned window capture is unavailable: `liveWindowTargetAtPoint`
  returns `nullopt` and the pin-window action reports that no capturable window was
  found.
- Global mouse gestures fall back to the unsupported backend. Global keyboard
  shortcuts work through `XGrabKey` on X11 sessions.
- Wayland sessions are detected and the screenshot feature family is gated off
  with a user-visible explanation; the verified desktop path is X11 with
  OpenGL 3.3 or newer (XWayland follows the X11 path on a best-effort basis).
- The GPU-resident edit pipeline is Windows-only. On Linux the viewer displays
  images through QRhi/OpenGL but resizes and exports with the CPU reference path
  and the isolated worker. `snow_image_viewer_edit_mode_performance` therefore
  reports a residency failure and is excluded with `-LE performance`.
- Wayland is unvalidated; the verified desktop path is X11 with OpenGL 3.3 or
  newer (XWayland follows the X11 path on a best-effort basis).
- Snow Shot's Windows/macOS integrations (File Explorer selection, printing,
  sharing/AirDrop, startup registration, updater, crashpad) fall back to their
  non-Windows stubs.
- Snow Shot writes structured JSON session logs on Linux under
  `~/.local/share/SnowShot/snow_shot/logs/<date>/`. Crashpad crash reporting and
  automatic upload remain Windows-only; diagnostics also go to stderr.
- Packaging beyond the plain `cmake --install` tree (DEB/RPM/AppImage,
  installer scripts), static Qt, and ARM64 are out of scope.
