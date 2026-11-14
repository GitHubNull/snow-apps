# Snow Apps Linux P5 移交文档（M2 录屏收尾 → M3c Crashpad / M4 验收发布）

- 创建时间：2026-11-15
- 仓库：`/home/wgh/dev/cross_platform_app/snow-apps`
- 分支 / 提交：`main` @ `1ee0ce5b`（M2c 点击/键盘特效已提交）；`origin/main` 同步状态以 `git status` 为准
- 远程仓库：`GitHubNull/snow-apps`（`mg-chao/snow-shot` 的公开 fork）
- 本文自包含。历史阶段：`docs-linux-handover.md`（P0/P1）、`docs-linux-p2-handover.md`（OCR/滚动）、`docs-linux-p3-handover.md`（质量收尾）、`docs-linux-p4-handover.md`（v1.4.8 收尾）；构建事实与 Known limitations 以 `docs-linux-build.md` 为准。

## 0. 给接手代理的第一段话

Linux X11 移植已完成 M0–M3b、M3d、M3e 和 M2 全部（M2a 采集编码 / M2b 录音 / M2c 点击·键盘·滚动特效），均已提交。用户在真实 `:0` 会话实测屏幕录制（含 H.264 编码 + AAC 音轨）通过。**不要重做已完成部分，不要跑全量测试套件。** 剩余只有两项：M3c Linux Crashpad 崩溃上报（§2.1）和 M4 全量验收/文档/i18n/发布（§2.2）。动手前读 §3（环境与禁区）和 §4（验证配方）。

## 1. 当前已验证状态（2026-11-15）

### 1.1 本轮（M2 录屏系列）提交序列（v1.4.8 tag 之后）

| 提交 | 内容 |
| --- | --- |
| `7360eaab` | fix(ci): install staged ocr assets in windows component trees（**修掉了 v1.4.8 Windows 发布失败的根因**，尚未经 CI 验证） |
| `dfca840a` | feat(linux): align desktop features and add the x11 recording backend |
| `de0a65e4` | feat(linux): add printing, autostart, appimage updates and packaging |
| `5d586d37` | build(linux): ship the fcitx5 qt6 input context and x264 encoding |
| `baf52f61` | fix(build): link x264-enabled ffmpeg without leaking bundled png symbols |
| `71909388` | feat(recording): capture pulseaudio system and microphone audio on linux |
| `be552815` | fix(recording): keep the toolbar above the recording area on x11 |
| `8a3339b4` | fix(capture): project cursor metadata for pixel-only native backends |
| `1ee0ce5b` | feat(recording): observe x11 raw input for click and keyboard effects（M2c） |

### 1.2 M2c 特效实现要点（避免回归）

- 新模块 `snow-crates/crates/snow-recording-effects/src/x11_observe.rs`：XI2 原始事件（raw button/key/motion）在根窗口上的选择、X keymap 加载/热重载（MappingNotify）、keysym→Windows VK 映射、修饰键快照（XQueryKeymap）、AltGr（ISO_Level3_Shift → 0xa5）。
- `mouse_hook.rs` / `keyboard_hook.rs` 的 Linux 平台分支已整体替换为 XI2 实现，产出与 Windows 相同的 `MouseClickObservation` / `KeyObservation`（VK 键位空间、scan 字段携带 X keysym 供 printable_label 布局正确取字符）。
- **Xephyr 三个坑（已写进代码注释，勿删）**：(a) XI 版本必须协商 2.2 而非 2.0，否则 Xephyr 不发 raw motion；(b) 同一根窗口必须同时选择非 raw 的 `XIEventMask::MOTION`，否则部分服务器（含 Xephyr）扣住 raw motion；(c) 绝对 warp（xdotool `mousemove X Y`）**永不**产生 raw motion，只有相对/真实移动才产生——测试脚本必须用 `mousemove_relative`。
- 滚轮按钮（detail 4/5）在 X11 有 press+release 两个事件，模型要求只报 press（release 被丢弃）。
- 根窗口 raw 选择在客户端间互斥（另一客户端占用会 BadAccess minor 46）；`connect()` 在组合 mask 失败时回退为纯 raw mask（点击/键盘仍覆盖）。
- 单测：`cargo test -p snow-recording-effects --lib` 101 个全过（含 3 个新增映射测试）。`cargo clippy -p snow-recording-effects --all-targets -- -D warnings` 零告警。

### 1.3 实测结论

- 用户 2026-11-15 在真实桌面实测：区域录屏 → x264 编码、AAC 音轨、MP4 封装正常，未发现问题。
- 特效事件链路（点击/滚动/按键/修饰键/AltGr/移动）由单测 + Xephyr 实时探针验证通过；**「特效确实画进 MP4 画面」的最终目视确认未做**（自动化手段不可靠后移交用户测试，用户反馈"大概测了没发现问题"）。M4 验收时建议让用户开设置里的「显示按键」「录制鼠标点击」录一段，肉眼确认圆环/按键浮层出现。

### 1.4 本机安装

- 当前安装版本仍是 `1.4.8`（`~/.local/opt/snow-shot`），**不含** M2 录屏系列。本文发布新版本后需替换安装树并真机验收（配方见 §4.4）。
- 版本核验：`strings -a -e l bin/snow_shot | grep -E '^1\.4\.[0-9]+$'`（不要信 session 日志里的 revision 字段）。

## 2. 剩余任务

### 2.1 M3c Linux Crashpad

目标：Linux 发布包接入崩溃转储（Windows 已有 Crashpad，macOS 有 bridge）。

入口与事实：

- `snow_shot/CMakeLists.txt:405` 注释明示现状："the crash-diagnostics feature has no Linux crashpad bridge"。
- `crash-diagnostics` 是 `snow_ocr_process`（Rust worker）的 cargo feature：当前仅 `WIN32 OR APPLE` 启用（`:392-398`），并注入 `SNOW_CRASHPAD_LINK_FILE` 环境指向生成的 `.rsp` 链接文件。Linux 要新增对称分支。
- C++ 侧：`snow_shot/src/diagnostics/crashcollector.cpp`、`diagnostics.cpp`；OCR worker 的桥：`snow-crates/crates/snow-ocr-process/native/diagnosticsbridge_macos.cpp`（macOS 版可作 Linux 蓝本）。测试：`snow_shot/tests/diagnostics_crash_tests.cpp`、`storage_directory_crash_capture_tests.cpp`。
- Windows 的 handler 二进制由 `SNOW_CRASHPAD_HANDLER`（crashpad_handler.exe）经 `:2331-2333` 拷入/安装；Linux 需要同等来源（查 vcpkg 是否有 crashpad port，或 FetchContent chromium crashpad；注意 ARM64/x64 双架构与 GPL 许可证清单同步——加依赖后必须跑 `scripts/collect-third-party-licenses.ps1` 并更新 `snow_shot/THIRD_PARTY_NOTICES.md`）。
- 打包：`de0a65e4` 引入的 AppImage/DEB 流程（`scripts/` 与 `.github/workflows/snow-shot-release-builds.yml`）必须把 handler 和 dump 目录一起带进去；AppImage 内的相对路径与 crashpad database 目录权限要实测。
- 验收：隔离显示里 `kill -SEGV` 一个测试实例，确认 dumps 目录产生 minidump 且日志记录路径。

### 2.2 M4 全量验收、文档、i18n 与发布

1. **验收**：按 `docs-linux-build.md` 的 Known limitations 逐项在用户真实 `:0` 会话过一遍（截图/录屏/贴图/滚动/OCR/翻译/打印/自启动/AppImage 更新/手势/输入法），重点补 §1.3 的特效目视确认。发现不符先定性（缺陷/设计如此/平台限制，见用户偏好），缺陷修掉再走发布。
2. **文档**：任何行为变更全量同步 `docs-linux-build.md` + `AGENTS.md`（Known limitations 与功能清单两处）。
3. **i18n**：M2 系列未新增 C++ 用户可见字符串（特效设置项的文案 Windows 侧已有），预期无 i18n 变更；仍须确认 `lrelease -fail-on-unfinished` 通过、无 unassigned context（自由函数里用全限定 `ClassName::tr`）。
4. **发布**：
   - 版本号惯例：patch 级递增（`CMakeLists.txt:21` `SNOW_SHOT_VERSION`），tag 名 `v1.4.X_snow-shot`（带 `_snow-shot` 后缀，annotated）。
   - 先确认 v1.4.8 的 Windows 失败已被 `7360eaab` 修复（看新 run 的 Windows job）。
   - **公开 fork Actions 节流**：若 dispatch/push tag 后无 run，去网页 Actions 页点 Re-enable（API 显示 enabled 也无效，仅网页可解）。
   - dispatch 用 `--ref main`（workflow 取 ref 处代码，修 bug 不必重打 tag）；失败 run 不建 draft release。
   - draft release 由用户手动发布。

## 3. 环境与禁区

### 3.1 禁区（违反会出事）

- **绝不向用户真实桌面 `DISPLAY=:0` 注入 XTEST/xdotool 合成键鼠**（曾锁死直连键盘迫使用户重启）。自动化 UI 输入只能在隔离 Xephyr `:10` 上做。
- 杀进程一律用**捕获的 PID**，绝不 `pkill -f <模式>`（模式会匹配调用 shell 自己的 cmdline，已两次自杀）。
- 绝不杀用户安装版实例（`~/.local/opt/snow-shot/bin/snow_shot`）；只杀 `./build/...` 启动的测试实例。
- 不跑全量测试套件（`ctest` 不过滤 / `cargo test --workspace`），除非用户点名。
- 提交只有用户要求时才做；commit ≠ push ≠ merge ≠ release。

### 3.2 构建/工具坑

- 系统 cmake 3.28 读不了 presets v4；必须用仓库 venv：`export PATH=/home/wgh/dev/cross_platform_app/snow-apps/.tools/venv/bin:$PATH`（cmake 4.2）。
- 构建预设：`build-linux-debug`；启动 `build/linux-debug/snow_shot/snow_shot`。ninja 目标名是 `snow_shot_full_rust_ffi_bundle_rust_build` 这类，直接构建 `snow_shot` 即可。
- Rust 依赖变更后 `snow_rust_ffi` 用 `--locked` 构建会失败，须先在该目录 `cargo check --offline` 刷新它的 `Cargo.lock`（CMake 构建带 FFMPEG_DIR 环境，手动裸 cargo check 可能报 FFmpeg pkg-config 错但 lock 仍会更新）。
- git push 走 `git@github.com:GitHubNull/snow-apps.git` 显式 SSH（https 握手被 RST）；gh 命令一律 `gh -R GitHubNull/snow-apps`。
- 网络代理：本机 7890 端口 HTTP 代理（cargo/git/curl 按需 `https_proxy`）。
- `cargo test -p <crate>` 会连编译 `tests/` 下的实时探针：均已加 `#[cfg(target_os = "linux")]` + `#[ignore]`，默认不会跑也不会在 Windows 编译失败，**保持这个门控**。
- 工作区现有未入库文件（保持现状，勿删勿提交）：`docs-linux-p4-handover.md`、`snow-crates/crates/snow-capture/tests/linux_region_cursor_probe.rs`、`snow-crates/crates/snow-cursor/tests/`、`snow-crates/crates/snow-recording-effects/tests/linux_input_observer_probe.rs`。

### 3.3 隔离显示配方

```bash
Xephyr :10 -screen 1280x800 -noreset &          # 记录 PID
DISPLAY=:10 metacity --sm-disable &              # 记录 PID
DISPLAY=:10 xclock &                              # 可选靶子窗口
DISPLAY=:10 ./build/linux-debug/snow_shot/snow_shot   # 启动测试实例（会写 ~/.config/SnowShot/snow_shot.conf）
# 截图：xwd -root -display :10 | xwdtopnm 2>/dev/null | pnmtopng > /tmp/shot.png
# 或 ffmpeg -f x11grab -i :10 -frames:v 1 /tmp/shot.png
```

## 4. 验证配方

### 4.1 门禁（改 Rust 后）

```bash
cd snow-crates
cargo fmt -p <crate> -- --check
cargo clippy -p <crate> --all-targets -- -D warnings
cargo test -p <crate> --lib
```

### 4.2 特效实时探针（需 Xephyr + xdotool）

```bash
DISPLAY=:10 cargo test -p snow-recording-effects --test linux_input_observer_probe -- --ignored
# 断言：左键按下/抬起坐标、滚轮只报 press、'a'→0x41、Shift 修饰键快照、AltGr、相对移动连续性
```

### 4.3 录屏 E2E（用户手测步骤，比自动化可靠）

1. `./build/linux-debug/snow_shot/snow_shot`（真实 :0 会话）。
2. 设置 → 录屏：勾「显示按键」「录制鼠标点击/滚动」。
3. 区域录屏，区域内左键/右键/滚轮/字母键/Shift+字母 各来几下，✕ 完成保存。
4. 播放 MP4 目视确认圆环、滚动提示、按键浮层。

### 4.4 新版本安装验收

替换 `~/.local/opt/snow-shot`（先备份旧树），`bin/snow_shot` 启动，§4.3 步骤过一遍 + `strings -a -e l bin/snow_shot | grep -E '^1\.4\.[0-9]+$'` 核对版本号。
