# MicroBenchX CoherencyLatency Multi-Platform Port

[中文](README.md) | English

A native macOS / Windows port of `CoherencyLatency.exe` from [MicroBenchX v1.0.5](https://github.com/CXWorld/MicroBenchX). The original benchmark modes, CLI options and semicolon-separated CSV output are preserved; the threading, affinity, timing and packaging layers are reimplemented.

> Unofficial port. Not affiliated with or endorsed by CapFrameX / CXWorld. The original project and `CoherencyLatency.cpp` are MIT-licensed, `Copyright (c) 2021 CapFrameX`.

## Download

Latest release: [v1.0.0](https://github.com/ddYIbb/MicroBenchX-CoherencyLatency/releases/tag/v1.0.0)

| Platform | Release asset |
|---|---|
| macOS (Universal 2) | [CoherencyLatency-macOS-Universal-v1.0.0.zip](https://github.com/ddYIbb/MicroBenchX-CoherencyLatency/releases/download/v1.0.0/CoherencyLatency-macOS-Universal-v1.0.0.zip) |
| Windows (x86 / x64 / ARM64) | [CoherencyLatency-Windows-x86-x64-ARM64-v1.0.0.exe](https://github.com/ddYIbb/MicroBenchX-CoherencyLatency/releases/download/v1.0.0/CoherencyLatency-Windows-x86-x64-ARM64-v1.0.0.exe) |

Both are single-file programs: a Universal 2 binary on macOS and a single launcher with embedded x86/x64/ARM64 payloads on Windows.

## Requirements

- **macOS Intel**: macOS 10.7 Lion or later (x86_64)
- **macOS Apple Silicon**: macOS 11.0 Big Sur or later (arm64)
- **Windows x86 / x64**: Windows Vista SP2 / 7 / 8 / 8.1 / 10 / 11
- **Windows on ARM64**: Windows 10 / 11 ARM64

macOS does not provide hard CPU pinning; matrix indices are logical slots, not reliable physical core IDs.

## Features

- Original `bounce` and `owned` benchmark modes.
- Original all-to-all matrix, semicolon CSV format, file naming and CLI options.
- Fixes the original stray-semicolon minimum bug: the minimum over all runs is used.
- Memory reduced from O(n² × runs) to O(n²); per-run sample storage is replaced by an online minimum and stack-allocated cache-line cells.
- `mach_absolute_time` on macOS and `QueryPerformanceCounter` on Windows; no dependency on newer `std::chrono` system symbols.

## Build

### macOS

Requires macOS 10.7+ (Intel) or 11.0+ (Apple Silicon), Xcode Command Line Tools and CMake 3.20+:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build --config Release -j
```

For Intel use `-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.7`. A prebuilt Universal 2 binary is at `prebuilt/CoherencyLatency`.

### Windows

From `windows/src` (Visual Studio C++ desktop workload required):

```bat
build.bat x86
build.bat x64
build.bat arm64
```

The single-file launcher build is documented in [windows/README.md](windows/README.md).

## Upstream and licence

- Upstream: [CXWorld/MicroBenchX](https://github.com/CXWorld/MicroBenchX), tag [v1.0.5](https://github.com/CXWorld/MicroBenchX/tree/v1.0.5)
- Original source: [source/MicroBenchX.Icl/CoherencyLatency.cpp](https://github.com/CXWorld/MicroBenchX/blob/v1.0.5/source/MicroBenchX.Icl/CoherencyLatency.cpp)
- MIT License, original copyright `Copyright (c) 2021 CapFrameX`; see [LICENSE](LICENSE).

Only `CoherencyLatency` is covered; `MicroBenchX.Ipc` and the other MicroBenchX tools are out of scope.
