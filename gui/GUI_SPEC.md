# CoherencyLatency 桌面 GUI 设计规范 v3（平台原生风格版）

> **v3 变更（用户要求）**
> 1. 每个平台的界面**各自重新设计**，不要互相照搬：Windows 走 Windows 11 / Fluent 风格，macOS 走 macOS 原生 AppKit 风格。
> 2. 需要保持一致的是**信息与行为**（有哪些内容、怎么测、怎么保存），不是像素级外观。
> 3. 窗口要小：默认 760×620 pt，最小 680×520 pt；**默认尺寸下 8×8 矩阵必须完整可见，不允许出现滚动条**。

## 0. 共同的信息骨架（两端都必须有，但用各自的方式呈现）

- 产品名 `CoherencyLatency` + 一行副标题，版本号 `1.2.0` 必须可见。
- 设备信息：逻辑线程数、缓存行大小、架构；物理核数与同核超线程分组（无 SMT 时省略）；测量对象说明；固定参数（`1,000,000 次 · 3 轮 · Bounce`）。
- 线程数选择（2..逻辑线程，默认 `min(logical, 8)`）。
- 操作：开始测试 / 停止 / 导出 CSV。
- 状态行：`就绪`、`测量中 x/y · T2↔T5 113.5 ns · 用时 3.2 s`、`完成 · N 个线程对 · 用时 x s`、`已停止 · 完成 x/y 个线程对 · 用时 x s`、`出错：…`。**只在每个线程对完成时刷新一次**。
- 进度指示（各平台用原生控件）。
- N×N 热力矩阵（对角线 x，未测 ·，完成填数值），热力色两端算法必须相同（见第 4 节）。
- 图例（低/高）+ 平均/最小/最大。
- 结果自动保存路径可见。

## 1. 通用约束

- 默认内容尺寸 **760 × 620 pt**，可缩放，最小 **680 × 520 pt**。启动居中。
- 页面外边距 **20 pt**，卡片间距 **12 pt**，卡片内边距 **16 pt**。
- **矩阵按可用宽高双向约束单元格尺寸**，默认尺寸下 8×8 完整可见：

```
availW = 卡内宽 - 行标签宽 - 4
availH = 卡内高 - 标题行 - 图例行 - 统计行 - 脚注行 - 间距
cell   = clamp(18, min(availW/n - gap, availH/n - gap), 40)
gap    = 3
```

  只有算出来小于 18 pt（即 n 很大）时才允许滚动。
- 高 DPI 必须清晰：Windows Per-Monitor DPI v2，macOS Retina 自动。
- 必须支持系统深色模式（两端自适应，浅色下也要正确）。
- UI 线程绝不阻塞：矩阵在工作线程跑，进度回主线程刷新。
- 只用 `gui/engine_api.h` 的接口；禁止修改 `gui/engine_api.h`、`gui/engine_api.cpp`、`windows/src/CoherencyLatency.cpp`、docs、README。

## 2. Windows：Windows 11 / Fluent 风格

用 Win32 + GDI/GDI+/DWM 实现，不引入第三方库：

- **字体**：`Segoe UI Variable Text`（回退 `Segoe UI`），标题可用 Display 变体；中文自动回退 `Microsoft YaHei UI`；数字用 `Cascadia Mono`（回退 `Consolas`）。
- **配色**：使用系统语义值而非硬编码。用 `DWMWA_USE_IMMERSIVE_DARK_MODE` 感知深浅色；浅色背景近似 `#F3F3F3`，深色近似 `#202020`；卡片浅 `#FFFFFF` / 深 `#2B2B2B`，圆角 **8 pt**，边框 1 pt `#E5E5E5` / `#3A3A3A`。
- **强调色**：从系统取（`DwmGetColorizationColor` 或注册表 `HKCU\Software\Microsoft\Windows\DWM\AccentColor`，取不到回退 `#0067C0`），主按钮用强调色 + 白字。
- **控件**：用真正的 Win32 控件（`BUTTON`、`COMBOBOX`、`PROGRESS_CLASS`），不要自绘伪控件；按钮 `BS_OWNERDRAW` 画出 Fluent 圆角（4 pt）与 hover/pressed/focus 三态，焦点矩形必须可见（键盘可用性）。
- **标题栏**：用 `DwmExtendFrameIntoClientArea` 或 `DWMWA_CAPTION_COLOR` 让标题栏与内容协调；保留系统按钮。
- **图标**：用 `tools/launcher-gui/app.ico`，同时作为窗口图标（`WM_SETICON`）。
- **键盘**：Tab 顺序正确，`Alt+S` 开始、`Alt+T` 停止、`Alt+E` 导出；`Esc` 停止运行中的测试。
- **高对比度**：`SystemParametersInfo(SPI_GETHIGHCONTRAST)` 为真时改用系统色，不自绘彩色背景。
- 矩阵：单元格圆角 4 pt、间距 3 pt，未测/对角线用 `#F0F0F0`（深色 `#333333`），数值白色粗体；行列表头用次要文字色。

## 3. macOS：原生 AppKit 风格

Objective-C++ / AppKit，不引入第三方库：

- **字体**：`[NSFont systemFontOfSize:weight:]`（自动就是 SF Pro + 中文 PingFang），不要硬编码 `PingFang SC`；数字用 `monospacedDigitSystemFontOfSize:weight:`。
- **配色**：全部用语义色——`NSColor.windowBackgroundColor`、`controlBackgroundColor`、`labelColor`、`secondaryLabelColor`、`tertiaryLabelColor`、`separatorColor`、`controlAccentColor`。**不要硬编码十六进制灰色**，否则深色模式会坏。
- **窗口**：标准 `NSWindow`（titled|closable|miniaturizable|resizable），`titlebarAppearsTransparent = YES` 或用 `NSVisualEffectView`（`NSVisualEffectMaterialWindowBackground` / `ContentBackground`）做背景材质；标题栏显示 `CoherencyLatency`。
- **控件**：真正的 `NSButton`（`.rounded` 或 `.push`，主按钮 `bezelColor = controlAccentColor`）、`NSPopUpButton`、`NSProgressIndicator`（bar 样式、indeterminate=NO）。不要自绘按钮。
- **图标**：如用到图标，用 `[NSImage imageWithSystemSymbolName:accessibilityDescription:]`（macOS 11+，旧系统文字回退）。
- **尺寸**：Auto Layout 或手动 layout 均可，但必须响应 `windowDidResize`；最小 680×520。
- **菜单**：App 菜单（关于/退出 ⌘Q）、编辑菜单（⌘C/⌘V 保留标准项）、窗口菜单（最小化 ⌘M）；「开始测试」⌘R、「停止」⌘.、「导出 CSV」⌘S。
- **行为**：`applicationShouldTerminateAfterLastWindowClosed` 返回 YES；退出时若在跑先 abort。
- 矩阵：单元格圆角 4 pt、间距 3 pt，对角线/未测用 `quaternaryLabelColor` 淡化底色，数值白色粗体；**热力色属于数据色，允许硬编码**（第 4 节），左右图例文字用语义色。

## 4. 热力色算法（两端必须完全一致）

取本次结果有效值的 min/max；`max-min < 1e-6` 时 t=0。

```
t = (v - min) / (max - min)
if (t < 0.5) s = t * 2          // #16B769 -> #F48011
else         s = (t - 0.5) * 2  // #F48011 -> #F83533
c0 = (22,183,105)  c1 = (244,128,17)  c2 = (248,53,51)
color = t < 0.5 ? lerp(c0,c1,s) : lerp(c1,c2,s)
```

图例渐变条也必须用这条曲线取 64 个采样点绘制，不要用系统线性渐变（会偏色）。

## 5. 交互（两端一致）

- 开始：禁用开始、启用停止；每个线程对完成时刷新状态行/进度/该格。
- 停止：请求中止，当前对跑完后停；已完成结果保留并做转置平均（只有单方向的格子镜像该值），未测格显示 `·`。
- 运行中禁用线程数控件。
- 完成后自动写 CSV 到 `Documents/CoherencyLatency`（Windows `%USERPROFILE%\Documents\CoherencyLatency`，macOS `~/Documents/CoherencyLatency`），不存在则创建；「导出 CSV」另存。
- 关窗时若在跑：先 abort、等工作线程退出再退出程序。

## 6. 验收

- 760×620 默认尺寸：8 线程矩阵**完整可见、无滚动条**，所有文字不裁切。
- 680×520 最小尺寸：布局不重叠，矩阵按公式缩小（仅超大 n 时才滚动）。
- 深色模式下无白底黑字残留，卡片与文字可读。
- Windows：x86/x64/ARM64 三个 /MT 单文件 exe，`/W4` 零告警；截图含就绪页与完成页。
- macOS：universal 单文件 + .app，`-Wall -Wextra` 零告警，无私有 API，arm64 minos 11.0 / x86_64 10.13。
- 报告：说明各自采用了哪些平台原生元素（字体、语义色、原生控件、快捷键、深色模式做法），与第 0 节信息骨架的对应关系，以及是否满足「默认尺寸无滚动条」。
