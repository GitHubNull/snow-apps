# Snow Apps Linux P3 功能转交文档（质量收尾 + 剩余功能补齐）

- 创建时间：2026-11-10
- 仓库：`/home/wgh/dev/cross_platform_app/snow-apps`（GitHubNull/snow-apps，私有）
- 分支：`main`。P2（OCR/翻译/QR/表格/LaTeX + 滚动截图）已全部完成、真机验收并提交（本文撰写时最新提交 `4519857f`，**推送状态以仓库为准**）
- 本文目标：让接手代理完成 Linux 适配的**质量收尾**（测试树/告警红线）与**剩余功能**（录屏、智能/窗口选区、实时贴图、划词翻译等）
- 先读 `docs-linux-handover.md`（P0/P1 环境、验收手册、禁区）、`docs-linux-p2-handover.md`（P2 完成状态与环境坑）、`docs-linux-build.md`（构建/现状）

## 0. 硬规则（与 P0/P1/P2 一致）

1. 只跑与改动相关的 `ctest -R` 过滤测试，**禁止全量测试**（除非用户明确要求）。
2. 不执行 `sudo`；系统安装整理成脚本交给用户跑。
3. 不擅自 push；commit ≠ push ≠ release。发版/tag 需用户明确确认。
4. Linux 改动用 `#if defined(Q_OS_LINUX)` / CMake `UNIX AND NOT APPLE` 分支隔离，不得回归 Windows/macOS。
5. 用户可见字符串必须走 `tr()`，上下文用**限定类名**，补齐 `snow_shot/i18n` 的 en_US/zh_CN/zh_TW 三目录，零 unfinished（`lrelease -fail-on-unfinished` 会在构建时拦截）。
6. UI 验收在真实 X11 桌面做，测完 `xdotool windowactivate <原窗口>` 把焦点还给用户；测试实例用隔离 `XDG_CONFIG_HOME`/`XDG_DATA_HOME`。
7. **验收前必须先重建**：`cmake --preset linux-debug`（CMakeLists 改动后必须显式重跑，ninja 自动再生成时缺 venv PATH 会因找不到 clang-format 失败——把 `.tools/venv/bin` 加进 PATH 再构建）+ `cmake --build --preset build-linux-debug --target snow_shot`，并用 `nm -C` 确认新符号进了二进制。
8. 禁止删除任何 bait/负测试文件；不得改用户 xrandr/显示配置；不碰 `~/.config` 下用户真实配置。

## 1. 现状核实（2026-11-10 实测）

### 1.1 已可用（P2 验收通过，不要重做）

- 文本识别（bundled `snow_ocr_process`，schema-3 `assets/ocr` manifest）、文本翻译、QR、表格、LaTeX：真机全部通过（生产 API `https://snowshot.top`；`linux-debug` 默认 `localhost:8080`，用 `api_configuration/server_url` 改）
- 滚动截图：Qt 帧源（根窗口抓取+选区裁剪）+ XTest 滚轮（`platform/linux/scrollinput.cpp`）+ XShape 输入排除（`x11inputregion.cpp`）。真机：垂直手动滚轮拼接 621×875 长图无重复行；自动滚动 `scrolling.wheel_dispatch status=0 (Posted)`；水平模式切换 `mode_changed mode=1` 管线正常重启
- 托盘/工具栏/快捷键入口全部恢复（除录屏、划词翻译、TranslateSelectedText）
- 关键日志事件：`scrolling.qt_create/first_frame/capture_progress/capture_summary/wheel_dispatch`；OCR `request.finished`/`ocr.finished`（`outcome`/`status` 字段齐全）

### 1.2 仍不可用（按用户要求保持禁用，做之前先与用户确认优先级）

- 录屏（`FeatureFamily::ScreenRecording` Linux false；需 Linux 编码后端，工程量大）
- 智能选区、窗口选区（`visual-region-detector` Rust crate 无 Linux backend）
- 实时贴图（`livewindownative.cpp` 桩：`liveWindowTargetAtPoint` 返回 nullopt）
- 划词翻译（`snow-selected-text` Rust crate 无 Linux backend）
- 打印、AirDrop、自动更新、Crashpad、DEB/RPM/AppImage 打包
- Linux ARM64 OCR（`screenshotocrassets.cpp` 的 `kPlatform` 目前只有 `linux-x64`，arm64 预留）

### 1.3 已登记的存量问题（非 P2 引入，接手后优先清）

1. **`snow-shot-scrolling-capture-settings-tests` 在 Linux 测试树链接失败**（连带 `--image-split-only` 变体）：目标直接编译 `formfields.cpp`/`themecolorscheme.cpp` 等设置源（经 `_scrolling_settings_test_sources`），又（传递）链入含同源 unity 成员的 `libsnow_shot_settings.a`，产生大量 multiple definition。该目标从未在本机测试树构建成功过。修法方向：测试目标改为只链 `snow_shot_settings` 不再直编重复源，或对重叠源清单去重/给 `snow_shot_settings` 关 unity。
2. **workspace 全量 clippy 在 Linux 失败**（P2 相关 crate 均零告警，勿动它们）：
   - stable：`snow-audio-recorder` `benchmark.rs` 引用被 cfg 掉的 `crate::convert`（模块门控不一致）；`snow-ui-selector` dead_code（`ElementRect::new`、`QueryControl` 字段）
   - nightly-2026-09-08：`snow-media` 6 处 `chunks_exact_to_as_chunks` 新 lint
   - 仓库现行口径（`scripts/check-rust.ps1`）：stable 跑 `--workspace --exclude snow-ocr-process`，nightly 单独 `-p snow-ocr-process`
3. **存量编译告警**（基线 `/tmp/p2-baseline-warnings.txt`，勿删）：`screenshotqrrecognitionservice.cpp:291 -Wchanges-meaning`；`-Wsubobject-linkage`（snow_draw_engine_qt 3 处 + aboutpagewidget/settingscustomwidget/settingspagewidget）；`-Winvalid-pch -mavx`（snow_draw_engine_qt）；minizip `mktemp`（vcpkg 第三方，大概率只能豁免）；Qt 部署器 sl/gl 等语言缺翻译告警（安装期）
4. **存量运行告警**：`Global mouse input initialization failed: 0`（`globalmousemanager.cpp:216`，每次启动）与 portal `QDBusError`（`qt.qpa.services`）。验收口径一直是"零新增"，这两条允许存在；若要清掉需先确认不影响 Windows/macOS
5. P1-E 遗留：剪贴板持久化行为、中文输入法（ibus/fcitx）在文字工具中的验证

## 2. 任务拆解（建议顺序）

### T1 修复 Linux 测试树 scrolling-capture-settings 链接（0.5 天，质量红线）

- 定位 `snow_shot/CMakeLists.txt:7660` 附近的 `snow-shot-scrolling-settings-test-runtime` 与 `snow-shot-scrolling-capture-settings-tests` 的源清单重叠，去重或改链库
- 验收：`ctest -R 'scrolling-capture-settings|scrolling-image-split'` 两用例在 Linux 通过；Windows/macOS 侧同名目标不受影响（改动只允许进 `UNIX AND NOT APPLE` 分支或共享清单的去重，不改测试语义）

### T2 Linux clippy/测试红线拉平（1 天）

- snow-audio-recorder：`benchmark.rs` 的模块门控与 `lib.rs` 对齐（`#[cfg]` 一致或 benchmark 模块整体门控）
- snow-ui-selector：dead_code 用 `#[expect(dead_code)]`/删除或补测试引用
- snow-media：nightly 新 lint `chunks_exact_to_as_chunks` 改写（或 `#[expect]`，注意要同时被 stable/nightly 接受）
- 验收：`cargo clippy --workspace --exclude snow-ocr-process --all-targets --all-features -- -D warnings`（stable）与 `RUSTUP_TOOLCHAIN=nightly-2026-09-08 cargo clippy -p snow-ocr-process --all-targets -- -D warnings` 全绿；`cargo test -p snow-stitch-images` 仍 118 passed

### T3 存量编译告警清零（1 天，纯质量）

- 逐条处理 §1.3.3（`-Wchanges-meaning` 用限定 typedef 名；`-Wsubobject-linkage` 把匿名命名空间类型提为文件级命名或改指针）
- vcpkg mktemp 与 Qt 部署翻译告警如无法根除，在 `docs-linux-build.md` 的验收口径里明确豁免并写明原因
- 验收：完整重配+重建 `build-linux-debug`，日志 `grep -icE "warning|警告"` 仅剩 §1.3.3 中明确豁免项

### T4 存量运行告警治理（0.5 天，先与用户确认口径）

- `globalmousemanager.cpp:216` 与 portal 注册失败：评估降级为 `qDebug`/一次性诊断事件是否合适（它们当前走 qWarning，出现在每次启动 stderr）
- 验收：启动会话 stderr/JSON 日志除明确豁免项外零 qWarning；不改变功能行为

### T5 水平滚动真机补验（0.5 天，需要横向可滚动页面）

- P2 只验证了水平模式**切换**与管线重启；未验证真实横向内容拼接
- 找横向可滚动目标（如超宽代码文件、横向时间线网页）；`Shift+滚轮` 手动或自动滚动水平模式（工具栏第二行第二个图标，`mode=1`）
- 验收：横向长图拼接正确（无重复/缺失列）；`scrolling.capture_summary` 宽度增长；XTest 水平按钮 6/7 方向正确（`scrollinput.cpp` 里 x>0→button 7，x<0→button 6）
- 注意 Qoder 编辑器纵向软换行不可横向滚；Burp/浏览器宽表可用

### T6 P1-E 剪贴板/输入法验证（0.5 天，见 `docs-linux-handover.md` §4/§8）

### T7+ 剩余功能（每个都是独立大项，**动手前必须与用户确认优先级与验收口径**）

- 录屏：需选型（PipeWire + FFmpeg？portal ScreenCast？），评估 `snow-recording-c` Rust 栈能否加 Linux 后端；注意 `FeatureFamily::ScreenRecording` 门与工具栏 `showScreenRecordButton`
- 智能选区/窗口选区：`visual-region-detector` 加 X11 后端（无 Mac/Win 私有 API 依赖，X11 有现成窗口枚举 `x11windowquery.cpp`）
- 实时贴图：`livewindownative.cpp` 桩替换；依赖窗口内容持续抓取（X11 可用 XShm/XComposite，注意性能）
- 划词翻译：`snow-selected-text` 加 X11 后端（PRIMARY selection + `x11windowquery`）；完成后移除 `globalshortcutmanager`/`settingscatalog` 里 TranslateSelectedText 的 Linux 禁用注释
- 打印/AirDrop/自动更新/Crashpad、DEB/RPM/AppImage：各自独立，先问用户

## 3. 环境与坑速查（P2 实测新增，全部踩过）

- 构建链：`.tools/venv/bin/cmake --preset linux-debug`；测试树 `build/linux-debug-tests`。ninja 自动再配置需要 `PATH=".tools/venv/bin:$PATH"`（clang-format 探测），否则 `icon manifest error` + configure 失败
- 单实例：`snow_shot` 二进程会把命令转发给旧实例后退出。`pkill -f '路径匹配'` 抓不到它（cmdline 是相对路径 `./snow_shot`）；用 `pkill -x snow_shot`（≤15 字符）。改完配置文件必须先杀旧实例再启动，否则旧实例退出时会把配置写回去
- X11 验收：
  - 工具栏位置随选区移动，**每次交互前从当前截图重新量坐标**；点旧坐标会点到保存/取消
  - 表格/QR 组合按钮的选项弹层是 **hover 触发**（悬停 1-2s），点击直接执行上次动作
  - 选区自带的信息面板/工具栏会挡住拖拽起点；从选区外角落起拖，中间插 2-3 个 mousemove 步骤
  - 滚轮注入用 `xdotool click 4/5`（上/下）；6/7 为水平
  - 截屏用 `DISPLAY=:0 ffmpeg -f x11grab -video_size 1920x1080 -i :0 -frames:v 1 out.png`（无 import/eog/tkinter）
  - 锁屏会中断一切 X11 操作（长会话注意），恢复后先 `xdotool getactivewindow` 确认状态
  - 滚动捕获长图底部的灰色带先对照原屏再定性——Qoder 终端卡片本来就是灰底，别误判污染
- i18n：新串手动补三个目录即可（构建期 lrelease 校验）；`docs-linux-p2-handover.md` 完成状态一节有实例
- 代理：`https_proxy=http://127.0.0.1:7890`；git https 被 RST 时改用 `git@` SSH 显式 URL push
- OCR 资产 staging 在每次构建自动重新生成 `assets/ocr/asset-manifest.json`——**破坏 manifest 做失败路径测试后，任何一次重建都会把它修回来**，别把"重建后 OCR 恢复"误判为 bug

## 4. 禁区

- 不得回退 P0/P1/P2 已有行为（快捷键、直连截屏、光标、窗口排除、Wayland 门禁、OCR bundled、滚动 Qt 帧源）来迁就新功能。
- 不得在未初始化子模块的仓库上放开会 FATAL 的 CMake 门控。
- 不得提交 `.tools/`、`build/`、临时截图/日志。
- 不删 bait/负测试文件；`snow-shot-scrolling-capture-exclusion-tests` 已按平台拆分（仅 Win/macOS 注册），不要"恢复"它在 Linux 的注册。
- 改 `snow-media`/`snow-ui-selector`/`snow-audio-recorder` 只做 lint/门控修复，不改运行行为；这些 crate 与 P2 无关但被红线覆盖。

## 5. 完成标准

- T1-T4：Linux 测试树滚动相关全绿；stable+nightly clippy 口径全绿；完整重建告警仅剩书面豁免项；运行 stderr 零新增 qWarning
- T5/T6：真机验收记录写回本文档完成状态一节
- T7+：每项独立提交、独立验收，完成后同步 `docs-linux-build.md` Known limitations、`AGENTS.md` Linux 段与本文档
- 全程：只跑过滤测试；commit ≠ push；文档随代码同步
