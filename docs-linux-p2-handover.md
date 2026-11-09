# Snow Apps Linux P2 功能转交文档（OCR/翻译/识别族 + 滚动截图）

- 创建时间：2026-11-09
- 仓库：`/home/wgh/dev/cross_platform_app/snow-apps`（GitHubNull/snow-apps，私有）
- 分支：`main`。P0+P1（构建/安装/全局快捷键/直连截屏/光标/窗口排除/Wayland 检测/多屏几何）已全部完成、真机复验并提交（最新提交 `e44f98aa`，**推送状态以仓库为准**）
- 本文目标：让接手代理在 Linux 上补齐**文本识别、文本翻译、滚动截图**（用户明确要求的三项），可选 QR/表格/LaTeX 识别
- 先读 `docs-linux-handover.md`（环境、验收手册、禁区）与 `docs-linux-build.md`（构建/现状）

## 完成状态（2026-11-10）

P2-A 全部完成并真机验收通过；P2-B 代码全部落地，真机滚动验收待解锁屏幕后补做。

- A1 OCR worker Linux 编译：`snow_ocr_process` 在 `linux-debug` 下构建，`--version` 输出 `linux-x86_64 protocol 7`；子模块 nightly clippy 零告警（提交 `95baefe8`、`e3938333` 系列）
- A2 OCR 资产 bundled 分发：`scripts/snow-shot-linux-ocr.py` + schema-3 `assets/ocr` manifest（`platform=linux-x64`），`core.tests` 与 asset 单测通过（`aac514e2`、`6357f0e8`）
- A3 运行时门禁解除：托盘/快捷键/工具栏恢复 OCR、翻译、QR/表格/LaTeX 入口；TranslateSelectedText 保持禁用（`e3938333`）
- A4 真机验收：OCR 中文识别、翻译（生产 API `snowshot.top`）、QR 解码、表格提取、LaTeX 提取全部通过；损坏 manifest 快速失败并按平台给出重装提示（修复了 Linux 误报 "for this Mac" 的文案缺陷，`93389ad0`）
- B1 Qt 滚动帧源：根窗口抓取 + 物理选区裁剪 + 容量 1 帧邮箱 + 重复判定，offscreen 测试 `snow-shot-scrolling-qt-source-tests`（`54df387a`）
- B2 XTest 滚轮 + XShape 输入排除：`snow_shot/src/platform/linux/scrollinput.cpp`、`x11inputregion.cpp`；自动滚动失败与 macOS 同样上报 capture error（`9a96a759`）
- B3 工具栏放开滚动截图入口（`91b0123d`）；**overlay 帧污染真机核查待做**
- B4 真机滚动拼接验收、安装树裸跑验收**待做**（验收窗口期机器锁屏，需用户解锁后补）
- 拆分记录：`snow-shot-scrolling-capture-exclusion-tests` 仅保留在仍使用原生流的平台（Windows/macOS）；Linux 由新的 Qt source 测试覆盖

## 0. 硬规则（与 P0/P1 阶段一致）

1. 只跑与改动相关的 `ctest -R` 过滤测试，**禁止全量测试**（除非用户明确要求）。
2. 不执行 `sudo`；系统安装整理成脚本交给用户跑。
3. 不擅自 push；commit ≠ push ≠ release。发版/tag 需用户明确确认。
4. Linux 改动用 `#if defined(Q_OS_LINUX)` / CMake `UNIX AND NOT APPLE` 分支隔离，不得回归 Windows/macOS。
5. 用户可见字符串必须走 `tr()`，上下文用**限定类名**（如 `snow_shot::app::ApplicationController::tr(...)`，自由函数里禁止短名 `QCoreApplication::translate`），并在 `snow_shot/i18n/modules.json` 登记后补齐 en_US/zh_CN/zh_TW 三个目录，零 unfinished。
6. UI 验收在真实 X11 桌面做，测完 `xdotool windowactivate` 把焦点还给用户原窗口；测试实例用隔离 `XDG_CONFIG_HOME`/`XDG_DATA_HOME`。
7. **主程序验收前必须先重建**：`cmake --preset linux-debug`（CMakeLists 改动后必须显式重跑，ninja 自动再生成缺 venv PATH）+ `cmake --build --preset build-linux-debug --target snow_shot`，并用 `nm -C build/linux-debug/snow_shot/snow_shot | grep <新符号>` 确认新代码真的进了二进制（P1 阶段曾因 8 小时前的旧二进制误判"功能失效"，查了一上午）。
8. 禁止删除任何 bait/负测试文件；不得改用户 xrandr/显示配置。

## 1. 现状核实（2026-11-09 实测）

- Linux 构建**不编译** `snow_ocr_process`：`snow_shot/CMakeLists.txt:399` 的 `if(WIN32 OR APPLE)` 门控整个 OCR worker 的 Rust 构建。
- 根因：`snow-crates/crates/simd-paddle-ocr-rs` 子模块**未初始化**（`git submodule status` 显示 `-` 前缀），且它是独立的 PaddleOCR 推理引擎，没有 Linux 构建验证。`cmake/SnowOcrToolchain.cmake:6` 会在子模块缺失时 `FATAL_ERROR`（该文件目前只在 WIN32/APPLE 分支被 include，Linux 上根本走不到）。
- OCR 工具链要求**钉死的 dated nightly**（`rust-toolchain.toml` 里 `channel = "nightly-YYYY-MM-DD"` 格式，SnowOcrToolchain.cmake:14 解析）。
- 运行时门禁（OCR 不可用时的人造表现，解除时都要同步还原）：
  - `snow_shot/src/storage/settingscatalog.cpp`（T5 提交 `62e63c22`）：`kLinuxUnavailableQuickActions` 数组移除了 ScreenshotOcr/ScreenshotOcrCopy/ScreenshotTranslation/TranslateSelectedText 等 8 个动作的托盘入口与绑定
  - `snow_shot/src/presentation/services/globalshortcutmanager.cpp`：`actionAvailable()` 的 Linux switch 同样屏蔽这些动作
  - `docs-linux-build.md` Known limitations 登记为不可用
- 滚动截图：UI 完整（`screenshot_toolpalette.cpp` 的 `Tool::ScrollingScreenshot`，工具栏"滚动截图"），但 Linux 没有捕获后端——滚动捕获在 Windows/macOS 走 Rust `snow-capture` 原生后端，Linux 只有 Qt `QScreen::grabWindow` 单帧路径（见 `docs-linux-build.md` "Linux capture path"）。
- 功能版本开关：`include/snow_shot/app/edition.h` 用 `SNOW_SHOT_ENABLE_TEXT_TRANSLATION / _QR_RECOGNITION / _TABLE_RECOGNITION / _LATEX_RECOGNITION / _IMAGE_CONVERSION` 编译期常量控制翻译/QR/表格/LaTeX 工具是否出现（`editionfeatures.h` 的 `editionActionToolAvailable`）。这些宏的定义来源接手时需沿 CMake 查清并保证 Linux 预设下值正确。

## 2. P2-A：OCR / 文本翻译（大项，建议先做这个的调研）

### 任务拆解

1. **子模块 Linux 移植（最大工作量）**：`git submodule update --init --recursive snow-crates/crates/simd-paddle-ocr-rs` 后尝试在 Linux 构建。需要确认：其 Cargo 依赖树在 linux x86_64 可编译（可能有 Windows-only 依赖或 SIMD 汇编假设）；nightly 工具链版本与仓库 Rust 1.97.1 的协同（它用自己的 rust-toolchain.toml，互不影响，但要确认 nightly 日期仍可用，`rustup` 下载慢的走本机代理 `https_proxy=http://127.0.0.1:7890`）。
2. **CMake 放开**：`snow_shot/CMakeLists.txt:399` 与 `:432` 的 `if(WIN32 OR APPLE)` 改成包含 `UNIX AND NOT APPLE`；`cmake/SnowShotMacOS.cmake:122` 附近的安装/部署分支核对 Linux 需要哪些对应物（`if(TARGET snow_ocr_process)` 的各处：`CMakeLists.txt:9250`（资源/模型）、`:10582`（依赖）、`:10921`（测试/宿主拷贝）、`:11244`（install）——这些已经是平台中性的 TARGET 判断，理论上放开构建即生效，逐个核对）。
3. **运行时装配**：OCR worker 进程是 `snow_add_rust_executable` 产出的独立可执行文件，随 app 目录部署（`:10925` 的拷贝逻辑）。确认 Linux 下 app 能找到它（参考 Windows 的查找逻辑，可能有平台路径假设）。
4. **解除运行时门禁**：按 §1 列的三处还原（settingscatalog 移除列表、globalshortcutmanager switch、featureavailability 如有）；`docs-linux-build.md` Known limitations 更新。
5. **翻译**：文本翻译 = OCR + 翻译 API 调用，`SNOW_SHOT_ENABLE_TEXT_TRANSLATION` 编译开关 + `api_configuration` 设置族。OCR 通了之后翻译主要验证 API 配置 UI 与网络层在 Linux 正常（vcpkg 静态 TLS 等已在 P0 解决）。

### 验收

- `snow_ocr_process` 目标在 `linux-debug` 预设下编译通过；`cargo clippy -D warnings`（在其 crate 内）不报错。
- 真机：F1 截图 → 文本识别出结果；翻译窗口出译文；托盘菜单恢复识别/翻译入口。
- 相关单测（`ctest -R 'ocr|translation'` 过滤）通过；新增行为的确定性 offscreen 测试。

### 风险

- simd-paddle-ocr-rs 是第三方推理引擎的魔改 fork，Linux 编译错误可能不少；若移植代价过大，**停下来向用户汇报选项**（如换用系统 OCR 引擎），不要硬啃超过一周的量。
- QR/表格/LaTeX 识别是同一 OCR worker 的不同模式（`editionRecognitionModeAvailable`），OCR 移植成功后低成本跟进，可作为 P2-A 的子任务。

## 3. P2-B：滚动截图

### 任务拆解

1. 调研 Windows/macOS 滚动捕获的调用面：Rust `snow-capture` 的滚动 API 在 C++ 侧的入口（从 `ScrollingScreenshot` 工具命令顺藤摸瓜到 FFI 层），弄清"滚动触发 + 多帧采集 + 拼接"的语义契约。
2. Linux 实现选型（二选一，推荐 A）：
   - A. **Qt/X11 实现**：对目标窗口用 XTest 假滚轮（或 `XSendEvent`）驱动滚动，每滚一步 `QScreen::grabWindow` 采一帧，图像层做重叠区拼接（现有 `snow_image` 有拼接/缝合工具可复用，`screenshotselectionshadowrenderer.cpp` 有轮廓拼接参考）。好处是不碰 Rust；坏处是窗口探测、滚动量控制、动画等待都要自己写。
   - B. 在 Rust `snow-capture` 加 Linux 后端：X11 抓帧已经在 FFI 边界外（Linux 走的是 Qt provider），这条路要新建 Rust→X11 依赖，和现有架构相反，不推荐。
3. 接入点：`screenshotcapturecoordinator.cpp` 的 Linux 分支目前只处理单帧；滚动是多帧 + 流式回调，需要扩展 coordinator 的 Linux 路径或走新的 workflow。注意与 `capturewindowshield`（自身窗口隐藏）组合：滚动期间应用窗口也必须保持隐藏。
4. 解除 UI 门禁：滚动截图工具在 Linux 的可用性开关（沿 `editionActionToolAvailable`/`actionToolAvailable` 查当前 Linux 的隐藏点）。

### 验收

- 真机对可滚动窗口（浏览器/编辑器）完整滚完并拼接出长图，接缝无重复/缺失行。
- 滚动失败（窗口不可滚/滚动中断）有明确用户提示，不静默。
- 单测：拼接算法用构造的多帧 fixture 验证（offscreen，不依赖真实滚动）。

## 4. 环境与坑速查

- 构建链：`.tools/venv/bin/cmake --preset linux-debug`，构建 `--preset build-linux-debug --target snow_shot`；测试树是独立的 `build/linux-debug-tests`（`linux-debug-tests` / `build-linux-snow-shot-tests` 预设）。
- 代理：`https_proxy=http://127.0.0.1:7890`（rustup/cargo/vcpkg/aqt 都认）；git https 被 RST 时走 `git@` SSH 显式 URL。
- Qt 只能用车载 `.tools/Qt/6.12.0/gcc_64`，系统 Qt 6.4 不行。
- X11 共存规矩：头文件保持 X11-free（`Bool/Status/None` 宏会毒化 Qt）；cpp 里 Qt 头在前、`#undef None True False` 在后；快捷键后端那种文件要 `SKIP_UNITY_BUILD_INCLUSION ON`。
- 符号隔离红线：`--exclude-libs,ALL` 必须保留；验收 `nm -D` 查 `png_` 导出为 0，否则 GTK 文件对话框会崩。
- 单实例陷阱：`snow_shot` 起第二个会转发给已有实例然后退出——验收新版本前必须先把旧进程 kill 掉，否则你以为在测新二进制其实在测旧的。

## 5. 禁区

- 不得回退 P0/P1 已有行为（快捷键、直连截屏、光标、窗口排除、Wayland 门禁）来迁就 P2。
- 不得在未初始化子模块的仓库上直接放开 `if(WIN32 OR APPLE)` 门控——会 FATAL_ERROR 卡住所有人的构建。
- 不得提交 `.tools/`、`build/`。
- 大改动前先 commit+push 当前 main 作为回滚锚点（用户已授权本次推送；后续 push 仍需逐次确认）。

## 6. 完成标准

- P2-A：Linux 预设下 `snow_ocr_process` 构建通过；真机 OCR/翻译可用；门禁还原；文档同步；相关过滤测试通过。
- P2-B：真机滚动截图出长图；单测覆盖拼接；文档同步。
- 全部：零新增编译警告；i18n 三目录无 unfinished；`nm png_` = 0；`docs-linux-build.md` / `AGENTS.md` / 两份 handover 文档事实同步。
