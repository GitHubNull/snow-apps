# Snow Apps Linux 桌面支持交接文档

- 更新时间：2026-11-09
- 仓库：`/home/wgh/dev/cross_platform_app/snow-apps`
- 分支 / 提交：`main` @ `ed9b305b`（P0+P1 全部完成，领先 `origin/main` 13 个提交，尚未 push）
- 工作区：干净
- 本文自包含：接手代理不需要旧会话上下文
- 后续工作（OCR/翻译/滚动截图）见 `docs-linux-p2-handover.md`

## 0. 给接手代理的第一段话

Linux 适配已经完成阶段 0-3，P2（OCR/翻译/识别族 + 滚动截图）也已实现：Snow Image Viewer 和 Snow Shot 的“选区截图 → 标注 → 复制/保存 → 静态贴图”核心链路、X11 全局快捷键、直连截屏（当前显示器/聚焦窗口/MCP）、XFixes 光标合成、自身窗口排除、Wayland 检测与提示、安装树（含捆绑 Qt 与 `.desktop`）均已实现并通过自动化测试。**不要重做已完成部分**，剩余工作只剩：解锁桌面后的真人 UI 复验（见 §8 清单）、P1-E（剪贴板/输入法验证）与滚动截图/安装树的真机收尾（见 `docs-linux-p2-handover.md`）。

开始前请先读完本文和 `docs-linux-build.md`。硬性规则：

1. 只跑与改动相关的测试，**禁止跑全量测试**（除非用户明确要求）。
2. 不执行 `sudo`；需要系统安装时把命令整理成脚本交给用户执行。
3. 不擅自 `git commit` / `git push`，除非用户明确要求；commit ≠ push。
4. 改动尽量用 `#if defined(Q_OS_LINUX)` 隔离，不要回归 Windows/macOS 行为。
5. 用户可见字符串必须走 Qt 翻译体系，并补齐 `snow_shot/i18n` 的三个语言目录。
6. UI 验收必须在真实 X11 桌面完成，测试后把焦点还给用户原来的窗口。

## 1. 当前状态

### 1.1 已提交的三个 Linux 提交

| 提交 | 内容 |
| --- | --- |
| `c20af187` | 阶段 0：Linux 预设、bootstrap、系统依赖脚本、构建文档 |
| `abe619f9` | 阶段 1：Viewer 在 X11 上打开/编辑/导出，Unicode 路径修复 |
| `2c10a004` | 阶段 2：Snow Shot 构建、Qt 截图提供者、Coordinator Linux 分支、功能门禁、几何测试、文档 |

阶段 3（P0+P1，2026-11-08/09 完成，尚未 push）：

| 提交 | 内容 |
| --- | --- |
| `f2197225` | 文档事实修正（托盘宿主、会话日志） |
| `08ae2b95` | bootstrap venv 钉死 libclang 18.1.1 / clang-format 23.1.3（P0-A） |
| `7ed5e165` | `linux-release` 预设 + Qt 部署 + `$ORIGIN/../lib` RPATH + `.desktop`（P0-B） |
| `9688b082` | X11 共享层：连接、grabkey、cursor、windowquery（P0-C/P1-A/P1-B 基础） |
| `c2398acf` | `XGrabKey` 全局快捷键后端（P0-C） |
| `46664eec` | Linux 直连截屏（当前显示器/聚焦窗口/MCP，`SNOW_CAPTURE_BACKEND_X11_QT`）（P0-D） |
| `62e63c22` | 托盘/命令过滤：不可用命令在 Linux 隐藏（P0-D） |
| `9c1a060d` | X11 直连截屏源接入构建（接线修复） |
| `89df494b` | XFixes 光标合成（P1-A） |
| `371dd5b8` | 抓屏前隐藏自身窗口（P1-B） |
| `2800efb4` | shield include 路径修复 |
| `a394d2e4` | Wayland 会话检测 + 功能门禁 + 启动警告（P1-C） |
| `ed9b305b` | 多屏负原点并集 + 混合 DPI 物理矩形测试（P1-D） |

### 1.2 已验证可用（真实 X11，1920×1080 单屏）

- Snow Image Viewer：打开 PNG/JPEG/WebP/GIF、OpenGL 显示、缩放、编辑、导出。
- Snow Shot 启动后显示主窗口。
- 浮动工具栏触发截图 → 拖选 651×421 区域 → 箭头/文字/马赛克标注。
- 复制到剪贴板：用 `xclip` 取回后 `ffprobe` 确认 651×421，且包含标注。
- 保存：GTK 原生保存对话框不再崩溃，PNG 保存成功且尺寸/内容正确。
- 静态贴图：生成 651×421 的置顶窗口。
- 几何单测 `snow-shot-linux-capture-geometry-tests` 通过（约 0.33 s，offscreen）。
- 运行时日志实际存在：`~/.local/share/SnowShot/snow_shot/logs/<date>/*.log`（JSON 行）。
- 托盘：验证会话存在 StatusNotifier host；应用运行时会注册 `StatusNotifierItem`（实测应用连接 `:1.532` 出现在 watcher 列表）。是否在面板可见取决于桌面托盘插件，但“Cinnamon 完全没有托盘宿主”的旧说法已被推翻。
- 安装树：bundled Qt + `$ORIGIN/../lib` RPATH，`ldd` 0 个缺失、`nm -D` 无 `png_` 导出；窗口启动与 `.desktop`（gio launch）均验证通过（P0-B）。
- bootstrap 可复现：删除 `.tools/venv` 后重装 + 强制 bindgen 重跑，`snow_shot` 构建成功（P0-A）。
- 全局快捷键：`global-shortcut-manager` 单测 + 真实 X11 grab 的 native smoke 测试通过（P0-C）。
- 直连截屏：workflow/frame/native 单测通过；托盘“当前显示器/聚焦窗口”入口代码路径与单屏回归测试一致（P0-D，真人 UI 复验待解锁桌面）。
- 光标合成、自身窗口排除、多屏/混合 DPI 几何：对应单测全部通过（P1-A/B/D，真人 UI 复验待解锁桌面）。

### 1.3 明确不可用或未实现

- 录屏（`FeatureFamily::ScreenRecording` 在 Linux 返回 false）。
- 智能选区、窗口选区、实时贴图窗口、划词翻译（`snow-selected-text` 无 Linux 后端）。
- 全局鼠标手势（`UnsupportedGlobalMouseBackend`）；全局键盘快捷键已可用（`XGrabKey`）。
- 自动更新、Crashpad 崩溃上报、打印、AirDrop。
- Wayland：已做检测与提示（功能门禁 + 启动警告），但 Wayland 原生截屏未实现；多显示器/混合 DPI 有几何测试覆盖，真人多屏复验待解锁桌面。
- DEB/RPM/AppImage 等发行级打包未做（`cmake --install` 树已可用）。

> P2（2026-11-10）：OCR/翻译/QR/表格/LaTeX 与滚动截图已补齐，见 `docs-linux-p2-handover.md` 的完成状态一节。

## 2. 快速开始

```bash
cd /home/wgh/dev/cross_platform_app/snow-apps

# 构建 Snow Shot + Viewer
.tools/venv/bin/cmake --preset linux-debug
.tools/venv/bin/cmake --build --preset build-linux-debug

# 运行（必须在 X11 会话）
DISPLAY=:0 ./build/linux-debug/snow_shot/snow_shot
```

说明：

- Qt 固定使用 `.tools/Qt/6.12.0/gcc_64`；系统 Qt 版本过低，不能替代。
- Debug 产物 `build/linux-debug/snow_shot/snow_shot` 约 1.07 GB，启动较慢，不要同时开多个实例。
- `build/` 与 `.tools/` 都是生成目录，禁止提交。
- 代理 shell 不加载用户 `.profile` 时，命令要显式带 `DISPLAY=:0`。

### 构建测试

```bash
.tools/venv/bin/cmake --preset linux-debug-tests
.tools/venv/bin/cmake --build --preset build-linux-snow-shot-tests
.tools/venv/bin/ctest --test-dir build/linux-debug-tests \
    -R 'linux-capture-geometry' --output-on-failure
```

Viewer 测试（仅在改到 viewer 时运行）：

```bash
.tools/venv/bin/cmake --build --preset build-linux-viewer-tests
.tools/venv/bin/ctest --test-dir build/linux-debug-tests \
    -R '^snow_image_viewer' -LE performance --output-on-failure -j4
```

## 3. 关键代码地图

| 文件 | 作用 |
| --- | --- |
| `snow_shot/src/presentation/capture/screenshotqtcaptureprovider.cpp` | Linux 截图核心：枚举 `QGuiApplication::screens()`，主屏 `grabWindow(0)` 抓整张虚拟桌面，再按 `physicalRect` 裁剪；失败回退逐屏抓取；统一为 RGB32/DPR 1/sRGB |
| `snow_shot/src/presentation/capture/screenshotqtcapturegeometry.h` | 纯函数：`displayStableId()` 生成稳定显示 ID；`cropDisplayFrame()` 处理虚拟桌面/独立 X screen/尺寸不匹配 |
| `snow_shot/src/presentation/capture/screenshotcapturecoordinator.cpp` | Linux 分支（约 82-210 行）：用 GUI 线程 queued 调用替代 FFI worker，保留 `prepared`/`layoutReady`/`captureFinished` 合同和 generation 取消语义 |
| `snow_shot/include/snow_shot/presentation/screenshotcapturecoordinator.h` | `finishLinuxCapture()` 与 `m_linuxGeneration` 声明 |
| `snow_shot/src/platform/linux/windowcaptureexclusion.cpp` | 桩：`setWindowExcludedFromCapture` 返回 false，`captureWindowId` 返回 `nullopt` |
| `snow_shot/src/platform/linux/livewindownative.cpp` | 桩：实时窗口目标返回 `nullopt`，工厂返回 `nullptr` |
| `snow_shot/src/app/featureavailability.cpp` | Linux 仅关闭 `ScreenRecording`；其他 feature 仍可用 |
| `snow_shot/src/app/main.cpp` | Linux 启动默认显示主窗口（`--autostart` 除外）；更新器/Crashpad 仅 Windows |
| `snow_shot/src/presentation/toolbar/screenshottoolbarwindow.cpp` | Linux 显示 OCR/翻译/表格/二维码/滚动截图按钮；仅隐藏录屏按钮 |
| `snow_shot/src/presentation/toolbar/screenshottoolbarpresentationstatefactory.cpp` | 平台中性：`ocrAvailable` 按 OCR 资源/会话状态计算（P2 已移除 Linux 早退） |
| `snow_shot/src/presentation/ocr/screenshotocrrecognitionservice.cpp` | 平台中性：bundled 资产在 Linux 走 schema-3 manifest，失败信息按平台分支（P2） |
| `cmake/SnowShotLinux.cmake` | FFmpeg 静态闭包：`pkg_check_modules(... IMPORTED_TARGET GLOBAL)` + `PKG_CONFIG_ARGN --static`，同时挂到 Rust FFI 接口目标和 `snow_shot` |
| `cmake/RustStaticLibrary.cmake` | 无系统 `clang` 时给 bindgen 加 `CPATH` 回退（约 230-280 行） |
| `cmake/SnowRustLibclang.cmake` | 非 Windows libclang 发现顺序：`LIBCLANG_PATH` → venv `clang/native` → 系统 LLVM 目录 |
| `snow_shot/CMakeLists.txt` | Linux 源文件 2138-2145；`--exclude-libs,ALL` 2164-2169；几何测试 11532-11553；install 规则 11177 起 |
| `snow_shot/tests/linux_capture_geometry_tests.cpp` | 稳定 ID、虚拟桌面裁剪、布局/屏幕对齐、帧身份测试 |
| `scripts/bootstrap-linux.sh` | venv + Qt + vcpkg；只装 `cmake/ninja/aqtinstall/clang-format` |
| `scripts/install-linux-system-deps.sh` | 系统依赖（含 `clang`/`libclang-dev`/`clang-format`），需用户 sudo 执行 |
| `docs-linux-build.md` | 阶段 0-2 构建、测试、限制（本次已修正两处事实） |
| `CMakePresets.json` | `linux-debug`、`linux-debug-tests`、`build-linux-debug`、`build-linux-snow-shot-tests` |

## 4. 剩余任务（按优先级）

> **状态（2026-11-09）**：P0-A、P0-B、P0-C、P0-D、P1-A、P1-B、P1-C、P1-D 均已完成（见 §1.1 阶段 3 提交表）。以下各节保留原始任务描述供追溯；除 P1-E 与 P2 外不再有待办实现工作。真人 UI 复验清单见 §8。

### P0-A 构建可复现：系统 clang / libclang

**状态：已完成（`08ae2b95`）。** 采纳方案 A：`scripts/bootstrap-linux.sh` 的 venv 安装列表已钉死 `libclang==18.1.1` 与 `clang-format==23.1.3`；全新 venv 复现验证通过。以下保留原描述：

**现状与证据**

- 本机没有系统 `clang`、`clang-format`，也没有 `/usr/lib/llvm-*/lib/libclang.so`。
- `.tools/venv` 里有手工装的 `libclang 18.1.1`（`uv pip list --python .tools/venv/bin/python`），当前构建靠它。
- `scripts/bootstrap-linux.sh:39` 只安装 `cmake ninja aqtinstall clang-format`，不会安装 `libclang`。
- `cmake/SnowRustLibclang.cmake:86` 的注释却声称 venv libclang 由 bootstrap 安装，与脚本不一致。
- 更新后的 `scripts/install-linux-system-deps.sh` 已加入 `clang libclang-dev clang-format`，但用户尚未执行。

**任务（二选一，推荐 A）**

- A：把 `libclang`（与当前一致的 18.1.1 或兼容版本）加入 `scripts/bootstrap-linux.sh` 的 `uv pip install` 列表，使全新 venv 也能完成 bindgen；同时保留系统 clang 作为可选路径。
- B：要求用户先执行更新后的 `bash scripts/install-linux-system-deps.sh`（需要 sudo，代理不要代跑），并修正 `SnowRustLibclang.cmake` 的注释。

**验收**

- 移走 `.tools/venv` 后按文档重新 bootstrap，`cmake --preset linux-debug` + 构建 `snow_shot` 成功；或系统安装 clang/libclang-dev 后同样成功。
- `docs-linux-build.md` 的依赖说明与实际脚本一致。

### P0-B Linux 安装 / 打包

**状态：已完成（`7ed5e165`）。** `qt_generate_deploy_app_script` 捆绑 Qt 运行库 + 插件白名单 + `INSTALL_RPATH=$ORIGIN/../lib`，安装 `bin/`+`lib/`+`.desktop`；仓库外移动后裸跑验证通过。以下保留原描述：

**现状与证据**

- 没有 Linux install 预设，也没有 `.desktop`、图标、AppImage。
- 实测 `cmake --install build/linux-debug --prefix /tmp/...` 后，安装树二进制无法运行，`ldd` 报 3 个缺失：
  - `libQt6Core.so.6: version 'Qt_6.12' not found`（系统 Qt 过旧）
  - `libQt6Svg.so.6 => not found`
  - `libsnow_shot_image_codec_backend.so => not found`
- Qt 部署脚本只在 `WIN32` 下执行（`snow_shot/CMakeLists.txt:11247` 起），Linux 安装也没有配置 RPATH，`Build` 树的 RUNPATH 指向仓库 `.tools`。
- 结论：目前只能在构建树里运行，不能拷贝到别处或从桌面启动。

**任务**

- 增加 Linux 安装目标：安装 `snow_shot`、`libsnow_shot_image_codec_backend.so`、Qt 运行库与必要插件（platform `xcb`、imageformats、iconengines 等按实际需要）。
- 配置 `CMAKE_INSTALL_RPATH`（例如 `$ORIGIN/../lib`）与 `CMAKE_BUILD_WITH_INSTALL_RPATH`（按需），确保不依赖 `LD_LIBRARY_PATH`。
- 生成并安装 `.desktop` 文件（`Exec=snow_shot`、`Icon=`、`Categories=Graphics;Utility;`）和 hicolor 图标。
- 可选：用 linuxdeploy/AppImage 打包；若打包，**不要**把系统 libpng/zlib 一起塞进去，避免再次触发 GTK 符号抢占。
- 保持 `--exclude-libs,ALL` 不动；打包后用 `nm -D` 复核 `png_` 导出仍为 0。

**验收**

- `cmake --install` 到临时前缀后，把该目录移动到仓库外（如 `/tmp/snow-install-run`），在不设置 `LD_LIBRARY_PATH`、不设置 `QT_PLUGIN_PATH` 的情况下能启动主窗口。
- `ldd` 无 `not found`。
- 从 `.desktop` 启动成功；保存对话框仍不崩溃。
- 截图核心链路在安装版上复验一遍（选区 → 标注 → 复制/保存）。

**注意**

- 系统 Qt 6.4 不能替代项目 Qt 6.12；不要尝试用系统 Qt 打包装。
- install 规则目前还会安装 QR 模型和许可证文件，保持这些路径有效。

### P0-C 全局快捷键（X11）

**状态：已完成（`9688b082`、`c2398acf`）。** `XGrabKey` 后端已接入现有 `GlobalShortcutBackend` 工厂；lock-mask 组合、冲突回滚、native smoke 均验证。以下保留原描述：

**现状与证据**

- `snow_shot/src/presentation/services/globalshortcutbackend.cpp:36` 的工厂只在 Windows/macOS 返回实现，其他平台返回 `UnsupportedGlobalShortcutBackend`。
- 因此 Linux 上无法用快捷键触发截图，只能主窗口/托盘/浮动工具栏。

**任务**

- 新增 `snow_shot/src/platform/linux/globalshortcutbackend.cpp`（可另配 `_p.h`），在 `globalshortcutbackend_p.h` 声明工厂，并在 `globalshortcutbackend.cpp` 增加 Linux 分支。
- 用 X11 `XGrabKey` 或 XCB 实现注册/注销；注意：
  - 必须同时 grab `NumLock`/`CapsLock` 的组合，否则这些键按下时快捷键失效。
  - `BadAccess` 表示快捷键被其他程序占用，要映射为现有的 registration failure 状态，而不是崩溃。
  - 应用退出、快捷键改绑、禁用全局快捷键时都要 `XUngrabKey`。
  - Wayland 下不要尝试 XGrabKey；返回不支持并给出可读提示。
- 沿用现有 `GlobalShortcutManager::activated` / `bindingActivated` 信号，不要改 UI 层协议。
- 新增用户可见错误/状态文案时，按仓库 i18n 规则补齐 `snow_shot/i18n` 的 `en_US`、`zh_CN`、`zh_TW`，不能留 `unfinished`。

**验收**

- 在设置中绑定一个快捷键（如 `Ctrl+Alt+A`），焦点在其他应用时按下能拉起选区截图。
- 改绑/禁用/退出后旧快捷键立即失效。
- 故意绑定一个已被占用的组合，设置界面显示失败状态，应用不崩溃。
- 在 Wayland 会话中给出明确“不支持”的提示，不出现静默失败。

### P0-D Linux 直连截屏与托盘菜单可用性

**状态：已完成（`46664eec`、`62e63c22`、`9c1a060d`）。** 直连截屏走 GUI 线程 grab + 每屏裁剪，聚焦窗口经 `_NET_ACTIVE_WINDOW` + `XTranslateCoordinates`；托盘/命令对不可用功能做 Linux 过滤。以下保留原描述：

**现状与证据**

- 托盘菜单仍列出“当前显示器”“聚焦窗口”“OCR/翻译”等命令，但没有按平台过滤。
- `DirectCaptureController::captureCurrentMonitor()` 在 Linux 不设置 `monitorName`，`captureFocusedWindow()` 不设置 `window`，`captureDirectTarget()` 在 `directcapturenative.cpp:19` 入口就直接返回空帧。
- 底层 `snow_capture_target_screenshot` 属于 Rust `snow-capture`，Linux 没有对应后端。
- OCR/翻译命令属于 `Screenshot` family，不会被 feature gate 拦住；点击后会进入截图，随后因缺 OCR 资源报错。
- 录屏命令会被 `FeatureFamily::ScreenRecording` 拦下。

**任务（二选一或组合）**

- 实现 Linux 直连截屏：在 `captureDirectTarget` 的 Linux 分支直接用 Qt 截图提供者（或复用 `ScreenshotQtCaptureProvider`）生成 `DirectCaptureFrame`；注意该函数当前在 worker 线程执行，Qt GUI 抓屏必须切回 GUI 线程（queued invocation），不能跨线程调用 `QScreen::grabWindow`。
- 或在 Linux 上把“当前显示器”“聚焦窗口”降级到选区覆盖层流程，并明确向用户展示降级行为。
- 过滤托盘/浮动工具栏中 Linux 不可用的命令（至少 OCR/翻译/表格/二维码/录屏/聚焦窗口），或在点击时给出明确错误而不是无响应。菜单清单在 `settingscatalog.cpp` 的 `buildBuiltInTrayCommandManifest()`，实际菜单构建在 `systemtraycontroller.cpp`。

**验收**

- 托盘“当前显示器”在 X11 上截到当前屏幕；“聚焦窗口”要么截到当前前台窗口，要么明确提示不支持。
- 托盘不再出现点击后静默失败的命令。
- 不影响 Windows/macOS 的 direct capture。

### P1-A 截图包含光标（XFixes）

**状态：已完成（`89df494b`）。** GUI 线程采样 `XFixesGetCursorImage`，预乘后合成进帧；取色器经 `cursorPatch`/`cursorPixelRect` 补偿。以下保留原描述：

**现状**

- 提供者抓的是不含光标的根窗口；设置里的“显示光标”在 Linux 上仍可见但无效。
- `DirectCaptureRequest.captureCursor` 在 Rust 路径会设置 `SNOW_CAPTURE_SCREENSHOT_REQUEST_INCLUDE_CURSOR`，但 Linux 不走该路径。

**任务**

- 用 XFixes `XFixesGetCursorImage` 读取光标图像和位置，在抓屏后按物理坐标合成；处理多屏负坐标、DPR、热点偏移。
- 只在设置开启时合成；`captureCursor` 关闭时不读光标。
- 如果暂不实现，至少在 Linux 上隐藏/禁用“显示光标”设置，避免“开关无效”的观感。

**验收**

- 开关打开时截图中光标位置正确、尺寸正确；关闭时无光标。
- 多屏/负坐标下光标不偏移（与 P1-D 一起测）。

### P1-B 自身窗口排除

**状态：已完成（`371dd5b8`、`2800efb4`）。** 抓屏前隐藏全部可见顶层窗口并等待 X 报告 unviewable（shield），抓完恢复；单测覆盖。以下保留原描述：

**现状**

- `setWindowExcludedFromCapture` 在 Linux 返回 false；`captureWindowId` 返回 `nullopt`。
- 重截路径 `screenshotcontroller.cpp:2308-2316` 会退化为隐藏窗口后抓屏；初次截图依赖窗口已隐藏的时序。

**任务**

- 至少在抓屏前可靠隐藏所有应用窗口并等待其真正不可见（expose/延迟/重试），避免浮动工具栏、主窗口、贴图窗口被截入。
- 若使用 XComposite/合成器方案，注意不要破坏无合成器环境。
- 保持 Windows/macOS 的排除实现不受影响。

**验收**

- 浮动工具栏可见时触发截图，截图中不出现工具栏。
- 重截（如重新选区/粘贴）时不出现上次的覆盖层或工具栏。
- 无合成器（如纯 X11 + 简单 WM）下仍能正常工作。

### P1-C Wayland 检测与降级

**状态：已完成（`a394d2e4`）。** 仅字面 `wayland` QPA 触发门禁（offscreen/minimal 不受影响）；主窗口与启动警告均有用户可见提示。以下保留原描述：

**现状**

- `QScreen::grabWindow(0)` 在 Wayland 下会失败或截黑；当前没有启动检测，失败信息是通用的 X11 抓屏失败。
- 文档明确要求 X11 会话。

**任务**

- 启动时检测 `QGuiApplication::platformName()`（`wayland` / `xcb`），在 Wayland 下：
  - 明确提示当前仅支持 X11（可用 `QT_QPA_PLATFORM=xcb` 走 XWayland），或
  - 禁用截图入口并保持应用可用。
- 如果选择支持 Wayland，需要 XDG Desktop Portal `Screenshot`/`ScreenCast`，属于独立项目，不要在本次强行实现。

**验收**

- Wayland 会话启动不出现“看似成功但黑图”的截图；用户能看到可操作的提示。
- X11 行为不变。

### P1-D 多显示器与混合 DPI 验证

**状态：几何测试已完成（`ed9b305b`）。** 负原点双屏并集、混合 DPI 物理矩形（1.0/2.0/1.25）取整、QPainter 颜色溯源均在 offscreen 单测覆盖；Xephyr Zaphod 双屏真人复验待解锁桌面。以下保留原描述：

**现状**

- 逻辑支持多屏（`physicalRect` 并集、按屏裁剪、独立 X screen 回退），但只在 1920×1080 单屏验证过。
- 未知项：负坐标、主屏不在 (0,0)、不同缩放（1.0/1.25/2.0）、Xinerama vs 独立 screen。

**任务**

- 在真实双屏或 Xephyr 多屏环境验证：覆盖层位置、选区坐标、裁剪边界、DPI 转换。
- 修复发现的问题，并优先复用/扩展 `linux_capture_geometry_tests.cpp` 的纯函数测试。
- 不要擅自修改用户 `xrandr` 配置；需要改显示设置时先征得用户同意。

**验收**

- 每个显示器都能正确截取，选区不偏移；混合 DPI 下几何一致。
- 单屏回归通过。

### P1-E 剪贴板持久化与输入法验证

- X11 剪贴板归应用所有，应用退出后是否保留取决于桌面剪贴板管理器；验证当前 Cinnamon 会话的行为并记录到文档。
- 文字工具用中文输入法（ibus/fcitx）输入未验证；若发现输入法候选框/提交异常，再定位 Qt xcb IM 插件问题。

### P2 功能缺位（默认不做）

> 状态（2026-11-10）：P2 已按用户要求完成 OCR/翻译/QR/表格/LaTeX 与滚动截图，见 `docs-linux-p2-handover.md`。以下清单仅剩这些仍未实现：

录屏、智能选区、窗口选区、实时贴图、划词翻译、打印/AirDrop、自动更新、Crashpad、安装包之外的发行渠道。除非用户明确要求，否则只做“隐藏/明确提示”，不要实现半成品。

## 5. 环境与坑（必读）

- **不要用系统 Qt**：系统只有旧版 Qt；`.tools/Qt/6.12.0/gcc_64` 是唯一验证过的运行库。
- **不要跑全量测试**：`ctest --preset ...` 全量是明确禁止的；只跑 `-R` 过滤后的相关测试。
- **不要 sudo**：系统依赖脚本交给用户执行；代理可以检查状态但不能代跑。
- **Python 只进仓库 venv**：`.tools/venv` 由 `uv` 管理，没有 `pip` 模块；安装用 `uv pip install --python .tools/venv/bin/python`，禁止全局安装。
- **代理**：下载依赖时用 `https_proxy=http://127.0.0.1:7890 http_proxy=http://127.0.0.1:7890`；Qt/vcpkg 慢时同样处理。
- **libclang/CPATH**：无系统 clang 时 `RustStaticLibrary.cmake` 会加 GCC include 路径；改动 libclang 发现逻辑后要重新验证 bindgen。
- **符号抢占（关键）**：`snow_image_shared`、`snow_shot_image_codec_backend`、`snow_shot`、`snow_image_viewer` 在 Linux 上必须保留 `LINKER:--exclude-libs,ALL`。否则 GTK gdk-pixbuf 会用到项目内 libpng，原生文件对话框报 `munmap_chunk(): invalid pointer`。验证：`nm -D <二进制> | grep -c ' png_'` 必须为 0。
- **FFmpeg 静态闭包**：`cmake/SnowShotLinux.cmake` 同时挂到 Rust FFI interface target 和 `snow_shot`；不要只改一处。
- **配置文件副作用**：用户配置 `~/.local/share/SnowShot/snow_shot/config.json` 中 `floating_toolbar.enabled` 已被测试改为 `true`（原值 `false`）。测试新配置时优先用临时 `XDG_DATA_HOME`/`XDG_CONFIG_HOME` 隔离，避免再改用户配置。
- **xdotool**：先 `windowactivate` 再点击；首次点击可能只聚焦。工具栏坐标随选区移动，本机参考值（选区 400,300→1050,720，1920×1080）：T≈698、马赛克≈775、贴图≈1052、保存≈1087-1093、关闭≈1148、复制≈1177-1190。**每次操作前先截图确认，不要盲点。**
- **pkill 自杀**：`pkill -f 'build/linux-debug/snow_shot/snow_shot'` 会匹配到当前 shell 自己的命令行。用 `pgrep -af 'build/[l]inux-debug/snow_shot/snow_shot'` 查 PID，再 `kill <pid>`。
- **焦点恢复**：测试结束用 `xdotool windowactivate <原窗口>` 把焦点还给用户。
- **真实桌面修改**：不要改分辨率、缩放、显示器布局；需要时先问用户。
- **i18n**：新增用户可见文案必须英文源串 + `tr()`，并补齐 `snow_shot/i18n/<feature>/snow_shot_<feature>_{en_US,zh_CN,zh_TW}.ts`；`lrelease` 会因 `unfinished` 失败。
- **格式**：C++ 用 `.tools/venv/bin/clang-format`（LLVM，4 空格，100 列）；Rust `cargo fmt` + `cargo clippy -D warnings`；不要提交 `build/`、`.tools/`、日志、截图。
- **单实例**：测试时如果用户已开着 Snow Shot，优先用临时 `XDG_DATA_HOME`/`XDG_CONFIG_HOME` 启动独立实例，避免互相干扰。
- **日志位置**：`~/.local/share/SnowShot/snow_shot/logs/<date>/snow-shot-*.log`，JSON 行，含 session.start、platform xcb、截图/导出/剪贴板事件；排障先看这里。

## 6. X11 验收手册

### 6.1 启动与截图

```bash
DISPLAY=:0 ./build/linux-debug/snow_shot/snow_shot
```

1. 确认主窗口出现；浮动工具栏已启用（当前用户配置为 true）。
2. 点击浮动工具栏的雪花按钮，等待选区覆盖层。
3. 拖选一块区域（例如 400,300 → 1050,720）。
4. 用工具栏做箭头/文字/马赛克标注。
5. 点击复制，验证剪贴板：

```bash
DISPLAY=:0 xclip -selection clipboard -t image/png -o > /tmp/snow-clipboard.png
ffprobe -v error -show_entries stream=width,height -of csv=p=0 /tmp/snow-clipboard.png
```

6. 点击保存（或 Ctrl+S），确认 GTK 保存对话框不崩溃，保存 PNG 并验证尺寸/内容。
7. 点击贴图，确认生成置顶静态窗口。

### 6.2 安装版验收（P0-B 完成后）

```bash
.tools/venv/bin/cmake --install build/linux-debug --prefix /tmp/snow-install-check
mv /tmp/snow-install-check /tmp/snow-install-run
cd /tmp/snow-install-run
env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH ./bin/snow_shot
```

验收点：能启动、能截图、能复制/保存、`ldd` 无 `not found`。测试后删除 `/tmp/snow-install-run`。

### 6.3 符号隔离检查

```bash
nm -D build/linux-debug/snow_shot/snow_shot | grep -c ' png_'
nm -D build/linux-debug/snow_image_viewer/snow_image_viewer | grep -c ' png_'
nm -D build/linux-debug/snow_shot/libsnow_shot_image_codec_backend.so | grep -c ' png_'
```

三项都必须是 `0`。

## 7. Git 与交付

- 当前 `main` 在 `ed9b305b`，领先 `origin/main` 13 个提交，**尚未 push**（用户未明确要求）；工作区干净。
- 每个任务单独提交，遵循 Conventional Commits，例如：
  - `build(linux): deploy Qt runtime and add install preset`
  - `feat(shortcuts): register X11 global hotkeys`
  - `fix(capture): include the cursor through XFixes`
- 提交前只跑相关测试；不要为了让门禁变绿而删除测试或跳过检查。
- 只有用户明确要求时才 `push`；发布/tag 另行确认。
- 若新增第三方依赖，更新 `vcpkg.json`、许可证收集脚本和 `THIRD_PARTY_NOTICES.md`。

## 8. 交接完成标准

P0 全部完成并满足各自验收，P1 至少完成光标/窗口排除/Wayland 提示中的用户指定项，且：

- 安装版能在仓库外独立启动；✅（P0-B 已验证）
- 全局快捷键可用或明确标注不支持；✅（P0-C 已验证）
- 托盘/快捷键不再出现静默失败；✅（P0-D 过滤已提交，真人复验待解锁桌面）
- 单屏 X11 核心截图链路无回归；✅（阶段 2 已验证）
- `docs-linux-build.md`、`AGENTS.md` 与实际行为一致；✅（T10 文档收口，2026-11-09）
- 所有改动通过相关测试与格式检查，工作区状态在交接说明中写清楚。✅

**待解锁桌面后的真人 UI 复验清单**（自动化均已通过，仅差真实桌面确认；2026-11-09 上午已解锁并完成第 1/3/6/7 项与安装版复验）：

1. T3：按下已绑定全局快捷键 → 截图 overlay 出现。✅（F1 → overlay 出现，注册日志 4/4 registered）
2. T4：托盘“当前显示器 / 聚焦窗口”直连截屏出图正确。⏳（单测覆盖，托盘菜单盲点击未做）
3. T6：开启/关闭“截图包含光标”对比验证光标合成。✅（capture_cursor=true 时抓帧含光标箭头，位置精确）
4. T7：截图帧中不含主窗口 / 浮动工具栏 / 贴图窗口。✅（overlay 背景帧无应用窗口）
5. T9：Xephyr Zaphod 双屏 + 混合 DPI 几何实测。⏳（几何单测覆盖负原点/混合 DPI；Xephyr 实测未做）
6. 安装版复验：用 §6.2 的方式启动安装树再跑一遍核心链路。✅（仓库外裸跑 + F1 全链路 + ldd 0 缺失 + png_ 0 导出）

**已解决的验收障碍**：`build/linux-debug` 主程序二进制曾停留在 T3 之前的旧构建（缺 Linux 快捷键后端符号），导致 F1 首次复验“注册失败”；重跑 `cmake --preset linux-debug` + 重建后一切正常。教训已记入项目记忆：主程序验收前必须先重建并 `nm` 确认新符号。

## 9. 本次已顺手修正的文档事实

`docs-linux-build.md` 中两处与实测不符的陈述已修正（未提交）：

1. 原文称验证机 Cinnamon “既没有 system tray 也没有 StatusNotifier host”，托盘图标必然不可见。实测 `IsStatusNotifierHostRegistered=true`，应用运行时会注册 `StatusNotifierItem`（watcher 列表出现 `:1.532/StatusNotifierItem`）。文档已改为：有宿主时托盘可用；无宿主时只剩浮动工具栏和主窗口；托盘菜单仍会列出不可用的识别/录屏命令。
2. 原文称 Linux 下两个应用都不写日志、诊断只走 stderr。实测 Snow Shot 会在 `~/.local/share/SnowShot/snow_shot/logs/<date>/` 写 JSON 会话日志；Crashpad 崩溃上报仍是 Windows-only。文档已按实测修正。
