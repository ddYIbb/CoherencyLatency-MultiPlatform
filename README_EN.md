# CoherencyLatency-MultiPlatform

[中文](README.md) | English

A multi-platform collection of ports of `CoherencyLatency`, the cache-coherency latency benchmark from [MicroBenchX v1.0.5](https://github.com/CXWorld/MicroBenchX):

- **macOS** — macOS 10.7 Lion or later
- **Windows** — Windows Vista SP2 / 7 / 8 / 8.1 / 10 / 11

This repository contains the cross-platform source tree and publishes both builds in [Releases](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases).

> This is an unofficial port and is not affiliated with, or endorsed by, CapFrameX or CXWorld.

`CoherencyLatency` measures how long a cache line takes to move between two logical CPUs. Two worker threads repeatedly perform an atomic compare-and-swap on the same cache line (`bounce`) or write their own cache lines and read the other thread's line (`owned`). Every ordered pair of logical CPUs is measured, and the result is written as a semicolon-separated matrix.

## Download

The latest release is [v1.0.0](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/tag/v1.0.0):

| Platform | Release asset |
|---|---|
| macOS | [CoherencyLatency-macOS-v1.0.0.zip](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/download/v1.0.0/CoherencyLatency-macOS-v1.0.0.zip) |
| Windows | [CoherencyLatency-Windows-v1.0.0.exe](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/download/v1.0.0/CoherencyLatency-Windows-v1.0.0.exe) |

The macOS download is a zip containing a native executable. If Gatekeeper blocks the first launch, right-click the file and choose **Open**, or remove the quarantine attribute with `xattr -dr com.apple.quarantine CoherencyLatency`.

The Windows download is a single `.exe`: it runs the native program matching the current Windows installation and deletes the temporary extracted file when it exits. No installer, nothing written to the registry, static CRT, and no Visual C++ runtime required. It runs on Windows Vista SP2 / 7 / 8 / 8.1 / 10 / 11. The first launch of a freshly downloaded file may take up to a few tens of seconds while Windows Defender scans it.

Chinese documentation is available for both platforms: [macOS 使用手册](docs/使用手册.md) ([PDF](docs/使用手册.pdf)) and [Windows 使用说明](docs/Windows-使用说明.md).

## Relationship to the original

This is a port, not a small patch. The benchmark idea, the `bounce` / `owned` modes, the all-to-all matrix and the semicolon-separated CSV format come from [MicroBenchX](https://github.com/CXWorld/MicroBenchX) by CapFrameX (MIT); the threading, affinity, timing and packaging layers were rewritten.

| | [MicroBenchX CoherencyLatency](https://github.com/CXWorld/MicroBenchX) | CoherencyLatency-MultiPlatform (this repository) |
|---|---|---|
| Platforms | Windows (official v1.0.5 release) | macOS, Windows |
| Timing | `QueryPerformanceCounter` | `mach_absolute_time` on macOS, `QueryPerformanceCounter` on Windows |
| CPU affinity | Windows thread affinity | `SetThreadAffinityMask` on Windows; best-effort QoS hint on macOS |
| Matrix memory | O(n² × runs) sample buffer | O(n²) matrix, online minimum, stack-allocated cache-line cells |
| Build | Visual Studio solution | CMake for macOS, MSVC batch scripts for Windows |

## What this port adds

**Native macOS build.** The timing layer uses `mach_absolute_time`, which works back to macOS 10.7, instead of newer `std::chrono` symbols. The released executable supports both older and newer Macs.

**Windows single-file launcher.** The launcher embeds the Windows native program as a resource. It extracts the program matching the current Windows installation to a temporary file, runs it, and removes it on exit. Releases are therefore a single `.exe`.

**Fixed minimum statistic.** The original source contains a stray semicolon in the minimum-selection loop, which makes the reported result depend on the last run rather than the fastest one. This port takes the minimum over every run, as intended.

**Lower memory use.** The matrix is stored as one O(n²) float array. Each pair's minimum is updated online while runs execute, so no O(n² × runs) sample buffer is allocated; the two cache-line cells used by a pair live on the stack.

**Preserved command line and output.** The original modes, option names, all-to-all matrix and semicolon-separated CSV output are unchanged. Existing scripts can keep using the same arguments and the default `core_latencies_<time>.csv` file name.

**macOS scheduling caveat.** macOS does not allow hard CPU pinning. The port asks for a best-effort affinity/QoS hint, but the scheduler can still move threads between performance and efficiency cores. Matrix indices are logical slots, not verified physical core IDs.

## Command line

| Option | Meaning |
|---|---|
| `-i, --iterations N` | Successful lock operations per thread. Default: 1000000. |
| `-r, --runs N` | Samples per ordered CPU pair. Default: 10. |
| `-m, --mode MODE` | `bounce` (same cache line, default) or `owned` (separate lines). |
| `--cpus N` | Use at most N logical slots. Default: all logical CPUs. |
| `--perf-only` | On supported Macs, limit logical slots to the detectable performance-core count. |
| `--csv PATH` | CSV output path. Default: `core_latencies_<time>.csv` in the current directory. |
| `-y, --yes` | Do not wait for Enter before and after the test. |
| `--no-progress` | Disable the progress bar. |
| `--no-affinity` | Disable the best-effort affinity/QoS hint. |
| `--list` | Print CPU information and exit. |
| `-h, --help` | Show help. |
| `--version` | Show version. |

Example:

```text
./CoherencyLatency --cpus 8 --iterations 1000000 --runs 10 --mode bounce --yes
```

The resulting CSV uses semicolons as separators and contains one row and one column per logical CPU slot.

## Building

Requirements:

- macOS 10.7 Lion or later
- Xcode Command Line Tools
- CMake 3.20 or newer
- For Windows: Visual Studio 2022 or newer with the **Desktop development with C++** workload, plus CMake 3.20 or newer for the launcher

No third-party libraries are needed; the project uses only the C++17 standard library and platform APIs.

### macOS

For Apple Silicon:

```bash
git clone https://github.com/ddYIbb/CoherencyLatency-MultiPlatform.git
cd CoherencyLatency-MultiPlatform

cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-arm64 --config Release -j
```

For Intel, use `x86_64` and deployment target `10.7`:

```bash
cmake -S . -B build-x86_64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.7
cmake --build build-x86_64 --config Release -j
```

To produce the universal binary shipped in Releases, build both configurations and combine them:

```bash
lipo -create build-x86_64/coherency_latency build-arm64/coherency_latency \
  -output CoherencyLatency
```

`bash scripts/build.sh` builds the Apple Silicon configuration with the same CMake flags.

### Windows payloads

From `windows\src`, run:

```bat
build.bat x86
build.bat x64
build.bat arm64
```

The three payloads are written to `windows/x86/CoherencyLatency.exe`, `windows/x64/CoherencyLatency.exe` and `windows/ARM64/CoherencyLatency.exe`. The scripts use `/MT` and target Windows Vista SP2 for x86/x64 and Windows 10 for ARM64. If Visual Studio is installed somewhere else, set `VSINSTALL` to its installation directory.

### Windows launcher

`tools/launcher` embeds the three payloads as RCDATA resources. Configure it only after all three payloads above have been built, because the resource compiler reads them at configure time:

```bash
cmake -S tools/launcher -B tools/launcher/build -A Win32
cmake --build tools/launcher/build --config Release
```

If the Visual Studio generator cannot find the compiler, use Ninja from a Visual Studio Developer Command Prompt instead:

```bash
cmake -S tools/launcher -B tools/launcher/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools/launcher/build
```

The result is `tools/launcher/build/CoherencyLatency.exe`, deliberately built as Win32 so the same file runs on every Windows machine: x86 and x64 natively, ARM64 through the built-in emulation layer.

## Third-party components

None. The port uses only the C++17 standard library and the platform APIs (`pthread` and Mach on macOS, Win32 on Windows). No external libraries need to be installed to build or run it.

## Licence

MIT — see [LICENSE](LICENSE).

Based on [MicroBenchX](https://github.com/CXWorld/MicroBenchX) by CapFrameX, licensed under the MIT Licence. The original copyright notice `Copyright (c) 2021 CapFrameX` is preserved in `LICENSE`.

This repository covers `CoherencyLatency` only; `MicroBenchX.Ipc` and the other MicroBenchX tools are out of scope.
