# CoherencyLatency-MultiPlatform

[English](README_EN.md) | 中文

[MicroBenchX v1.0.5](https://github.com/CXWorld/MicroBenchX) 中 `CoherencyLatency` 缓存一致性延迟测试的多平台移植合集：

- **macOS** — macOS 10.7 Lion 或更高版本（Intel 与 Apple Silicon 通用二进制）
- **Windows** — Windows Vista SP2 / 7 / 8 / 8.1 / 10 / 11（x86 / x64 / ARM64）

本仓库包含跨平台源码，并在 [Releases](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases) 中发布 macOS 与 Windows 两个平台的成品。

## 系统兼容性

两个成品都不需要安装任何运行库，拷过去就能跑：

| 平台 | 成品 | 最低系统 | 支持的 CPU / 指令集 |
|---|---|---|---|
| macOS | `CoherencyLatency-macOS-v1.0.0.zip` | Intel Mac：macOS 10.7 Lion（2011）；Apple Silicon Mac：macOS 11.0 Big Sur（2020） | 同一个 Universal 2 可执行文件，同时包含 x86_64 和 arm64（覆盖 M1/M2/M3/M4）切片，旧 Intel 和新 Apple Silicon Mac 无需选择版本 |
| Windows | `CoherencyLatency-Windows-v1.0.0.exe` | x86 / x64：Windows Vista SP2（2009）；ARM64：Windows 10 | 单文件启动器自动匹配 x86、x64、ARM64 原生程序；x86 版按最基础的 32 位指令集编译（连 SSE2 都不要求），x64/ARM64 为各自架构的基准指令集 |

几点说明：

- **不挑指令集。** 源码是纯 C++17，不使用 SSE/AVX/NEON 等扩展指令集，原子操作直接用各架构自带的原子指令，任何 x86、x64、ARM64 CPU 都能执行。
- **不装运行库。** Windows 版静态链接 CRT；macOS 版只依赖系统自带的 `/usr/lib/libSystem.B.dylib`。
- **兼容旧系统的方式。** macOS 版不依赖较新系统才有的 C++ 运行库符号，10.10+ 才有的 QoS API 通过运行时 `dlsym` 动态获取、老系统自动跳过；Windows 版链接 `SUBSYSTEM 6.0` 并静态链接 CRT。
- **ARM64 Windows** 上单文件启动器（本身是 x86）通过系统内置的模拟层运行，内部的 ARM64 原生程序直接执行，不经过模拟。
- **架构自适应，不用选版本。** macOS 一个文件双架构，Windows 一个文件三架构，复制过去即可；`--version` 和 `--list` 可用来确认程序在你的机器上正常启动。

> 非官方移植项目，与 CapFrameX / CXWorld 没有隶属或背书关系。

`CoherencyLatency` 测量一条缓存行在两个逻辑 CPU 之间传递的延迟。两个工作线程反复对同一条缓存行执行原子比较交换（`bounce`），或者各自写自己的缓存行、再读取对方的缓存行（`owned`）。程序会测量所有有序逻辑 CPU 对，并输出分号分隔的矩阵。

## 下载

最新版本：[v1.0.0](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/tag/v1.0.0)

| 平台 | Release 文件 |
|---|---|
| macOS | [CoherencyLatency-macOS-v1.0.0.zip](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/download/v1.0.0/CoherencyLatency-macOS-v1.0.0.zip) |
| Windows | [CoherencyLatency-Windows-v1.0.0.exe](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/download/v1.0.0/CoherencyLatency-Windows-v1.0.0.exe) |

macOS 版下载后解压得到一个可执行文件。如果首次打开被 Gatekeeper 拦截，请右键选择“打开”，或执行 `xattr -dr com.apple.quarantine CoherencyLatency` 清除隔离属性。

Windows 版是一个单文件 `.exe`：启动时自动选择与当前 Windows 匹配的原生程序，退出后删除临时释放的文件。不需要安装，不写注册表；静态链接 CRT，不需要安装 Visual C++ 运行库。支持 Windows Vista SP2 / 7 / 8 / 8.1 / 10 / 11。新下载的 exe 首次运行时，Windows Defender 可能会扫描几秒到几十秒，之后启动很快。

详细的中文使用说明见 [macOS 使用手册](docs/使用手册.md)（[PDF](docs/使用手册.pdf)）和 [Windows 使用说明](docs/Windows-使用说明.md)。

## 与原项目的关系

这是一个移植版本，不是小补丁。测试思路、`bounce` / `owned` 两种模式、全矩阵输出和分号 CSV 格式来自 CapFrameX 的 [MicroBenchX](https://github.com/CXWorld/MicroBenchX)（MIT）；线程、亲和性、计时和打包层则针对各平台重写。

| | [MicroBenchX CoherencyLatency](https://github.com/CXWorld/MicroBenchX) | CoherencyLatency-MultiPlatform（本仓库） |
|---|---|---|
| 平台 | Windows（官方 v1.0.5 发布版） | macOS、Windows |
| 计时 | `QueryPerformanceCounter` | macOS 用 `mach_absolute_time`，Windows 用 `QueryPerformanceCounter` |
| CPU 亲和性 | Windows 线程亲和性 | Windows 用 `SetThreadAffinityMask`；macOS 使用尽力而为的 QoS 提示 |
| 线程 | 每次 run 都新建两个线程 | 每个核心对复用两个线程，逐轮同步 |
| 矩阵内存 | O(n² × runs) 采样缓冲 | O(n²) 矩阵，在线取最小值，缓存行对象放在栈上 |
| 构建 | Visual Studio 解决方案 | macOS 用 CMake，Windows 用 MSVC 批处理脚本 |

## 本移植版新增的内容

**原生 macOS 构建。** 计时使用 `mach_absolute_time`，从 macOS 10.7 起就可用，不依赖更高版本系统的 `std::chrono` 符号。发布的可执行文件兼容新旧 Mac。

**Windows 单文件启动器。** 启动器把 Windows 原生程序作为资源嵌入同一个 exe。运行时释放匹配当前系统的程序到临时文件，执行完毕后删除。因此只需要一个可执行文件。

**修复最小值统计。** 原版源码在最小值判断循环里多了一个分号，导致结果实际取的是最后一次 run，而不是最快的 run。本移植版改为在所有 run 中取最小值。

**更低的运行开销。** 每个核心对只创建两个线程，用逐轮的启动闸门复用它们跑完所有 run，线程创建/销毁不再混进测量；计时改用整数高精度时钟（Windows `QueryPerformanceCounter`，macOS `mach_absolute_time`），不再受毫秒精度 `ftime` 限制。以 8 个槽位、10 次 run 为例，整轮耗时从 139 ms 降到 54 ms；`--cpus 32` 从 2914 ms 降到 1612 ms。

**降低内存占用。** 矩阵只保留一个 O(n²) 的浮点数组。每一对 CPU 的最小值在 run 执行过程中在线更新，不再分配 O(n² × runs) 的采样缓冲；每对 CPU 使用的两个缓存行对象放在栈上。32 槽位 50 次 run 时峰值工作集 4292 KB，原版为 4428 KB，且原版会随 `--runs` 继续增长。


**测量数值与原版的对齐情况。** 优化只降低整轮耗时与内存占用，不会改变测得的每操作延迟。在与原版源码同机交叉实测中（4 槽位、每 run 100 万次迭代、10 次 run），本移植版与原版重编译程序的矩阵数值差距在 0.1–2.4 ns 之间，随会话波动，处于环境噪声（线程在处理器组之间迁移可造成 ±2–3 ns 漂移）范围内。数值上的少量差异来自两处刻意的修正：统计量取全部 run 的最小值（原版因分号笔误实际输出最后一次 run），计时改用高精度时钟（原版为毫秒级 `ftime`）。如果需要与原版逐格对齐的输出，可以预期个位数纳秒级的抖动，这是测量噪声而非系统偏差。

**保留命令行和输出格式。** 原版的两种模式、参数名、全矩阵输出和分号 CSV 格式保持不变；原有脚本可以继续使用相同参数和默认的 `core_latencies_<time>.csv` 文件名。

**macOS 调度限制。** macOS 不支持硬性 CPU 绑定。移植版会请求尽力而为的亲和性/QoS 提示，但调度器仍可能在性能核与能效核之间迁移线程。矩阵下标是逻辑槽位，不是经过验证的物理核心编号。

## 命令行

| 选项 | 含义 |
|---|---|
| `-i, --iterations N` | 每个线程成功完成锁操作的次数。默认 1000000。 |
| `-r, --runs N` | 每个有序 CPU 对的采样次数。默认 10。 |
| `-m, --mode MODE` | `bounce`（同一缓存行，默认）或 `owned`（各自独立的缓存行）。 |
| `--cpus N` | 最多使用 N 个逻辑槽位。默认使用全部逻辑 CPU。 |
| `--perf-only` | 在支持的 Mac 上，把逻辑槽位限制为可检测到的性能核数。 |
| `--csv PATH` | CSV 输出路径。默认在当前目录生成 `core_latencies_<time>.csv`。 |
| `-y, --yes` | 测试前后不等待回车。 |
| `--no-progress` | 关闭进度条。 |
| `--no-affinity` | 关闭尽力而为的亲和性/QoS 提示。 |
| `--list` | 打印 CPU 信息后退出。 |
| `-h, --help` | 显示帮助。 |
| `--version` | 显示版本。 |

示例：

```text
./CoherencyLatency --cpus 8 --iterations 1000000 --runs 10 --mode bounce --yes
```

生成的 CSV 使用分号分隔，每行和每列各对应一个逻辑 CPU 槽位。

## 编译

环境要求：

- macOS 10.7 Lion 或更高版本
- Xcode Command Line Tools
- CMake 3.20 或更高版本
- Windows 需要 Visual Studio 2022 或更新版本，并勾选 **使用 C++ 的桌面开发** 工作负载；单文件启动器还需要 CMake 3.20 或更高版本

项目不依赖第三方库，只使用 C++17 标准库和平台 API。

### macOS

Apple Silicon：

```bash
git clone https://github.com/ddYIbb/CoherencyLatency-MultiPlatform.git
cd CoherencyLatency-MultiPlatform

cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-arm64 --config Release -j
```

Intel 改用 `x86_64` 和部署目标 `10.7`：

```bash
cmake -S . -B build-x86_64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.7
cmake --build build-x86_64 --config Release -j
```

要生成 Release 中的通用可执行文件，先分别完成两次构建，再合并：

```bash
lipo -create build-x86_64/coherency_latency build-arm64/coherency_latency \
  -output CoherencyLatency
```

`bash scripts/build.sh` 会用相同的 CMake 参数构建 Apple Silicon 版本。

### Windows payload

在 `windows\src` 目录运行：

```bat
build.bat x86
build.bat x64
build.bat arm64
```

三个 payload 分别生成到 `windows/x86/CoherencyLatency.exe`、`windows/x64/CoherencyLatency.exe` 和 `windows/ARM64/CoherencyLatency.exe`。脚本使用 `/MT` 静态链接 CRT，x86/x64 目标为 Windows Vista SP2，ARM64 目标为 Windows 10。如果 Visual Studio 不在默认路径，请设置 `VSINSTALL` 环境变量指向安装目录。

### Windows 单文件启动器

`tools/launcher` 会把三个 payload 作为 RCDATA 资源嵌入。请务必先完成上面的三个 payload 构建，再配置启动器，因为资源编译器在配置阶段就要读取这些文件：

```bash
cmake -S tools/launcher -B tools/launcher/build -A Win32
cmake --build tools/launcher/build --config Release
```

如果 CMake 的 Visual Studio 生成器找不到编译器，请在 Visual Studio 开发者命令提示符中改用 Ninja：

```bash
cmake -S tools/launcher -B tools/launcher/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools/launcher/build
```

产物是 `tools/launcher/build/CoherencyLatency.exe`。它刻意按 Win32（x86）编译，这样同一个文件能在所有 Windows 机器上运行：x86 和 x64 原生执行，ARM64 通过系统内置的模拟层运行。

## 第三方组件

无。项目只使用 C++17 标准库和平台 API（macOS 的 `pthread` 与 Mach API，Windows 的 Win32 API），编译和运行都不需要安装第三方库。

## 许可协议

MIT，详见 [LICENSE](LICENSE)。

本项目基于 CapFrameX 的 [MicroBenchX](https://github.com/CXWorld/MicroBenchX)（MIT 许可）修改而来，原版权声明 `Copyright (c) 2021 CapFrameX` 保留在 `LICENSE` 中。

本仓库只涉及 `CoherencyLatency`，不包含 `MicroBenchX.Ipc` 及其他 MicroBenchX 工具。
