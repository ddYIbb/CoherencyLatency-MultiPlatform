# MicroBenchX CoherencyLatency 多平台移植版

[English](README_EN.md) | 中文

这是 [MicroBenchX v1.0.5](https://github.com/CXWorld/MicroBenchX) 中 `CoherencyLatency.exe` 的原生 macOS / Windows 移植版。保留原版的两种测试模式、命令行参数和 CSV 输出格式，重新实现了线程、亲和性、计时和分发层。

> 非官方移植项目，与 CapFrameX / CXWorld 没有隶属或背书关系。原项目及其 `CoherencyLatency.cpp` 使用 MIT License，版权归 `Copyright (c) 2021 CapFrameX`。

## 下载

最新 Release：[v1.0.0](https://github.com/ddYIbb/MicroBenchX-CoherencyLatency/releases/tag/v1.0.0)

| 平台 | Release 文件 |
|---|---|
| macOS（Universal 2） | [CoherencyLatency-macOS-Universal-v1.0.0.zip](https://github.com/ddYIbb/MicroBenchX-CoherencyLatency/releases/download/v1.0.0/CoherencyLatency-macOS-Universal-v1.0.0.zip) |
| Windows（x86 / x64 / ARM64） | [CoherencyLatency-Windows-x86-x64-ARM64-v1.0.0.exe](https://github.com/ddYIbb/MicroBenchX-CoherencyLatency/releases/download/v1.0.0/CoherencyLatency-Windows-x86-x64-ARM64-v1.0.0.exe) |

两个平台都是单文件程序：macOS 是 Universal 2 可执行文件，Windows 是一个内嵌 x86/x64/ARM64 三个 payload 的启动器。

## 系统要求

- **macOS Intel**：macOS 10.7 Lion 或更高（x86_64）
- **macOS Apple Silicon**：macOS 11.0 Big Sur 或更高（arm64）
- **Windows x86 / x64**：Windows Vista SP2 / 7 / 8 / 8.1 / 10 / 11
- **Windows on ARM64**：Windows 10 / 11 ARM64

macOS 版没有硬性 CPU 绑定能力，矩阵中的编号只是逻辑槽位，不是可靠的物理核心编号；需要严格 core-to-core 测量请使用原版 Windows 程序或 Linux 的 `taskset` 环境。

## 功能

- 保留原版两种测试模式：
  - `bounce`：两个线程竞争同一条 cache line 的 CAS；
  - `owned`：两个线程分别写自己的 cache line，再读取对方的 line。
- 保留原版 all-to-all 矩阵、分号分隔 CSV、默认文件名和 CLI 参数。
- 修复原版统计代码中的 stray-semicolon 最小值 bug：现在取所有 run 的最小值，而不是最后一个 run。
- 内存从 O(n² × runs) 降为 O(n²)，采样单元改为栈上对象，避免每个 run 的堆分配。
- macOS 版使用 `mach_absolute_time`，Windows 版使用 `QueryPerformanceCounter`；两者都不依赖 `std::chrono` 的高版本系统符号。

## 目录结构

```text
MicroBenchX-CoherencyLatency/
├── src/main.cpp              # 跨平台 CoherencyLatency 源码
├── CMakeLists.txt             # macOS CMake 工程
├── scripts/build.sh           # macOS 构建脚本
├── prebuilt/CoherencyLatency  # macOS Universal 2 预编译文件
├── windows/
│   ├── src/                   # Windows 原生 payload 源码与构建脚本
│   └── launcher/              # 单文件 launcher 源码与 payload
├── docs/                      # 中文使用手册（md/html/pdf）
└── LICENSE
```

## 从源码构建

### macOS

需要 macOS 10.7+（Intel）或 11.0+（Apple Silicon）、Xcode Command Line Tools 和 CMake 3.20+：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build --config Release -j
```

Intel 构建把架构和部署目标改为：

```bash
-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.7
```

也可以直接运行 `bash scripts/build.sh`。预编译文件可直接检查：

```bash
chmod +x prebuilt/CoherencyLatency
./prebuilt/CoherencyLatency --list
```

### Windows

在 `windows/src` 目录中运行（需要 Visual Studio 的 C++ 桌面开发工具）：

```bat
build.bat x86
build.bat x64
build.bat arm64
```

三个原生 payload 生成到 `windows/x86`、`windows/x64`、`windows/ARM64`。单文件 launcher 的构建方法见 [windows/README.md](windows/README.md)。

## 上游与许可证

- 上游：[CXWorld/MicroBenchX](https://github.com/CXWorld/MicroBenchX)，tag [v1.0.5](https://github.com/CXWorld/MicroBenchX/tree/v1.0.5)
- 原始文件：[source/MicroBenchX.Icl/CoherencyLatency.cpp](https://github.com/CXWorld/MicroBenchX/blob/v1.0.5/source/MicroBenchX.Icl/CoherencyLatency.cpp)
- 许可证：MIT，原版权声明 `Copyright (c) 2021 CapFrameX`；本项目是 derivative work，详见 [LICENSE](LICENSE)。

本仓库只涉及 `CoherencyLatency`，不包含 `MicroBenchX.Ipc` 及其他 MicroBenchX 工具。
