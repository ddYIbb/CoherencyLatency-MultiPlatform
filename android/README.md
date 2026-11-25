# CoherencyLatency — Android 原生移植（命令行 + 图形界面 APK）

[MicroBenchX v1.0.5](https://github.com/CXWorld/MicroBenchX) 中 `CoherencyLatency` 的 Android 原生移植版，提供命令行与 APK 图形界面两种形态。
算法核心（bounce / owned 两种模式、每个核心对多 run 取最小值、对称平均、分号 CSV 输出）与原版一致，
只把 Windows 的线程 / 亲和性 / 计时层替换为 POSIX + bionic 实现。

- 上游项目：<https://github.com/CXWorld/MicroBenchX>（tag v1.0.5，MIT，Copyright (c) 2021 CapFrameX）
- 原文件：`source/MicroBenchX.Icl/CoherencyLatency.cpp`
- 版本：`1.2.0`
- 成品：命令行版 `CoherencyLatency-Android-v1.2.0.zip`（四个可执行文件）与图形界面版 `CoherencyLatency-Android-UI-v1.2.0.apk`（app/ 目录构建）
- 系统下限：**Android 4.1（API 16）**；32 位 ABI 按 API 16 编译，64 位 ABI 按 API 21 编译（bionic 的 64 位 ABI 从 21 起才有）

## 支持的目标

| ABI | 工具链 target | 产物 | 适用设备 |
| --- | --- | --- | --- |
| `arm64-v8a` | `aarch64-linux-android21` | `build/CoherencyLatency-arm64-v8a` | 2016 年后的主流手机 |
| `armeabi-v7a` | `armv7a-linux-androideabi16` | `build/CoherencyLatency-armeabi-v7a` | 32 位老设备（Android 4.1+） |
| `x86` | `i686-linux-android16` | `build/CoherencyLatency-x86` | 32 位 x86 模拟器 / Atom 平板 |
| `x86_64` | `x86_64-linux-android21` | `build/CoherencyLatency-x86_64` | 64 位 x86 模拟器 / Chromebook |

minSdk 16（Android 4.1 Jelly Bean），NDK r21e（clang 9，是最后一个还能编 API 16 的 LTS 版本）。四个产物都是 PIE 可执行文件，
只依赖 `libc.so` / `libm.so` / `libdl.so`，已用 `-static-libstdc++` 静态链接 C++ 运行库，
不需要在设备上额外安装任何库，也不需要 root。产物已用 `-s` strip，体积 239,960 / 128,756 / 239,364 / 264,736 B。

## 图形界面版 APK（app/）

`app/` 是同一测量核心的 Android GUI 封装：JNI 层 `jni/clbench_jni.cpp` 复用 bounce/owned、线程复用和 O(n²) 矩阵，
Java 层只负责固定参数、进度回调和矩阵显示。成品 `CoherencyLatency-Android-UI-v1.2.0.apk`
（minSdk 16，内含 arm64-v8a / armeabi-v7a / x86 / x86_64 四个 `libclbench.so`）。

- 固定默认参数：全部逻辑核心、1,000,000 次、3 轮、Bounce；
- 测量中实时显示进度与矩阵（未测 `·`，对角线 `x`，绑核失败 `N/A`），可中途停止并保留部分结果；
- 完成后自动保存 CSV 到 `getExternalFilesDir(null)/results/`，`导出 CSV…` 走系统 SAF 手动保存；
- 不申请任何权限（包括网络权限），不需要 root。

**多形态适配。** 同一份 APK（versionCode 5，533,655 B，SHA-256
`9f6d3c9ad0fc02140ae48a974931358b3beaf3ccfe2c568415a8f32e563db31f`）在四种形态下自动切换布局：

| 形态 | 判定 | 布局 |
| --- | --- | --- |
| 手机 | 宽度 < 600dp | 单列，标题 → 设备信息 → 按钮 → 进度 → 矩阵 → 导出 |
| 平板 | 宽度 ≥ 600dp 且横屏 | 左右两栏，左栏操作、右栏矩阵 |
| 圆形手表 | `UI_MODE_TYPE_WATCH` 或 ≤280dp | 首屏优先放开始/停止，设备信息和矩阵在下方滚动；圆形安全边距 |
| Android TV | Leanback 特性或 `UI_MODE_TYPE_TELEVISION` | 1.7 倍字号、两栏、按钮可聚焦，D-pad 全程可操作 |

已在模拟器上验证 1080×1920 手机、1600×1000 平板、454×454 圆形手表、1920×1080 Android TV；
TV 启动时焦点落在「开始测试」，运行中落到「停止」，完成后落到「导出 CSV…」。
调试构建可以用 `adb shell am start ... --es force_profile watch_round`（或 `tv`）强制切换形态，
正式发布构建会忽略这个 extra。

无 Gradle 构建：需要 NDK r21e、build-tools 31.0.0、platforms/android-31、JDK 17，在 `app/` 目录执行：

```bash
python build_apk.py
```

脚本依次编译四个 ABI 的 `.so`（每个都 strip）、生成图标、aapt2 编译资源、javac + d8 生成 dex（`--min-api 16`）、打包、
zipalign 对齐、apksigner 调试证书签名，最终输出到 `../CoherencyLatency-Android-UI-v1.2.0.apk`。
用户说明见 `docs/APK使用说明.md`（也有 `.html` / `.pdf`，含界面截图）。

## 命令行构建

需要 Windows / Linux / macOS 主机 + NDK r21e。推荐用 Python 驱动脚本（不依赖 `cmd` 的引号规则）：

```bash
python build_cli.py          # 一次编完四个 ABI
```

`build_cli.py` 里的 `TARGETS` 决定编哪些 ABI，`NDK` / `BIN` / `SYSROOT` 是 NDK r21e 的路径。
`build-android.bat` 保留同样的四个目标（CRLF 换行），但在 MSYS/Git Bash 里用 `cmd //c` 调用
只会输出 cmd 头不执行，所以日常构建用 `build_cli.py`。

手工编译等价命令（注意 `--sysroot` 必须写 Windows 盘符路径，写 MSYS 的 `/d/...` 会找不到 `jni.h`）：

```bat
"%NDK%\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" ^
  --target=aarch64-linux-android21 ^
  --sysroot="D:/ndk21/android-ndk-r21e/toolchains/llvm/prebuilt/windows-x86_64/sysroot" ^
  -std=c++17 -O3 -fPIE -pie -pthread -static-libstdc++ -Wall -Wextra -s ^
  src\main.cpp -o build\CoherencyLatency-arm64-v8a
```

32 位 ARM 与 32 位 x86 额外需要 `-latomic`（64 位原子操作在 32 位 ABI 上走 libatomic 的帮助函数）。

打包成品与文档：

```bash
python make-android-zip.py
```

会生成带 Unix 权限位（四个 ELF 为 0755、文档为 0644）的 `CoherencyLatency-Android-v1.2.0.zip`。
`build/SHA256SUMS` 由 `tools/gen_cli_sums.py` 生成。

## 安装到设备

### Termux（日常使用）

```bash
mkdir -p ~/bin
cp CoherencyLatency-arm64-v8a ~/bin/cl
chmod +x ~/bin/cl
~/bin/cl --list
```

Termux 里 `$HOME` 可写，CSV 默认落在当前目录。手机建议从 `--cpus 2` 或 `4` 开始。

### adb（调试用）

```bash
adb push build/CoherencyLatency-arm64-v8a /data/local/tmp/cl
adb shell chmod 755 /data/local/tmp/cl
adb shell "cd /data/local/tmp && ./cl --list"
adb shell "cd /data/local/tmp && ./cl -y --cpus 2 -i 100000 -r 2"
```

`/data/local/tmp` 是 adb 默认可写目录，普通应用进程访问不到，适合临时压测。
注意 `adb shell` 的初始工作目录是 `/`（只读），不指定 `cd` 时程序写 CSV 会失败——
移植版对这种情况会自动回退到 `$TMPDIR` 或 `/data/local/tmp` 并在输出中说明真实写入路径。

## 用法

命令行与原版一致：

```text
-i, --iterations N   每个线程的成功操作次数（默认 1000000）
-r, --runs N         每个核心对的采样次数（默认 10）
-m, --mode MODE      bounce（默认）或 owned
    --cpus N         最多使用 N 个逻辑槽位
    --perf-only      Apple 专用，Android 上无效果
    --csv PATH       CSV 输出路径
-y, --yes            跳过运行前后的 Enter 等待
    --no-progress    关闭进度条
    --no-affinity    关闭亲和性提示
    --list           只打印 CPU 信息
-h, --help           帮助
    --version        版本
```

CSV 格式与原版 v1.0.5 完全一致：分号分隔，首列 `Core i`，对角线 `x`，数值 6 位有效数字。

## Android 上的限制

- **亲和性只是建议**。移植版用 `sched_setaffinity(gettid(), ...)` 请求绑核，但 big.LITTLE 调度器、
  cgroup 和厂商内核都可能覆盖掩码。矩阵的行列号是**逻辑槽位**，不是经过验证的物理核心编号。
- **命令行版与 APK 都校验绑核结果**。每个 worker 在开始前用 `sched_getaffinity` 核对掩码、用 `sched_getcpu()`
  确认落点，每轮测量前后再复核一次；任一 worker 未绑住时，该核心对（含转置格）在矩阵与 CSV 中写 `N/A`，
  终端另打印 `Warning: N core pair(s) could not be pinned ...`，其余核心对照常完成（退出码仍为 0）。
- **单对失败不牵连整张矩阵**。每轮等待首轮上限 20 秒，之后按上一轮实测时间放宽（至少 5 秒）；
  某一轮卡住或时钟不前进时只把这一对记为 `N/A` 并重试一次，不再因为一对超时就整张矩阵作废。
- **不汇报物理核数**。Android 没有稳定的拓扑接口，`Physical CPUs` 与 `Logical CPUs` 取同一个值
  （模拟器上会额外标注 `(emulator)`）。
- **会热降频**。手机持续满载几十秒就会降频，测量值随之上升。探索性测试建议
  `--cpus 2` / `--cpus 4` 配 `-i 100000 -r 2`，正式测量前让设备冷却。
- **计时依赖 `CLOCK_MONOTONIC`**，跨核迁移不影响，但分辨率取决于内核（通常几十 ns 量级）。
- **32 位 ARM 数值偏高**。armeabi-v7a 上 64 位原子不是无锁指令，走 `libatomic` 帮助函数，
  只适合做可用性验证。

## 源码结构

单一文件 `src/main.cpp`，与 macOS / Windows 移植版共享同一套算法与 CLI 解析，平台差异集中在：

| 位置 | Android 分支 |
| --- | --- |
| `now_ns()` | `clock_gettime(CLOCK_MONOTONIC)` |
| `read_cpu_info()` | `sysconf(_SC_NPROCESSORS_CONF/ONLN)` 取较大值（4.x 的 CONF 返回 1）、`_SC_LEVEL1_DCACHE_LINESIZE`、`__system_property_get` |
| `configure_current_thread()` | `sched_setaffinity(gettid(), ...)` + `pthread_setname_np` |
| `print_cpu_info()` / `print_usage()` / `main()` | Android 文案与大核小核提示 |
| CSV 写入 | 只读工作目录时回退到 `$TMPDIR` / `/data/local/tmp` |

## 目录

```text
android/
├── src/main.cpp                 单一源文件
├── build_cli.py                 四个 ABI 的 Python 构建脚本（推荐）
├── build-android.bat            同上，bat 版（CRLF）
├── make-android-zip.py          生成带 Unix 权限位的发布 zip
├── app/                         APK 图形界面版源码（JNI + Java + 构建脚本）
├── docs/使用手册.md             命令行版中文使用手册
├── docs/使用手册.html           同上（网页版）
├── docs/使用手册.pdf            同上（打印版）
├── docs/APK使用说明.md          APK 版使用说明（含截图）
├── docs/APK使用说明.html        APK 版使用说明（网页版）
├── docs/APK使用说明.pdf         APK 版使用说明（打印版）
├── docs/screenshots/            APK 界面截图
├── README.md
└── LICENSE
```

编译产物 `build/` 与发布 zip 不入库，成品在 [Releases](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases)。

## 许可

MIT，见 [LICENSE](LICENSE)。原版权声明归 CapFrameX。