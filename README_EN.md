# CoherencyLatency-MultiPlatform

[中文](README.md) | English

A multi-platform collection of ports of `CoherencyLatency`, the cache-coherency latency benchmark from [MicroBenchX v1.0.5](https://github.com/CXWorld/MicroBenchX):

- **macOS** — macOS 10.7 Lion or later (universal binary for Intel and Apple Silicon)
- **Windows** — Windows Vista SP2 / 7 / 8 / 8.1 / 10 / 11 (x86 / x64 / ARM64)
- **Android** — Android 4.1 (API 16) or later (four ABIs for the CLI build; one universal APK for the GUI build)
- **HarmonyOS** — HarmonyOS 5.0.0 (API 12) or later (phones / tablets / 2-in-1, arm64-v8a)

This repository contains the cross-platform source tree and publishes macOS, Windows, Android and HarmonyOS builds in [Releases](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases). The HarmonyOS release is an unsigned HAP plus full source and must be signed locally; the other three platforms are separately built binaries.

## System compatibility

None of the downloads requires installing any runtime — copy it over and it runs:

| Platform | Release asset | Minimum OS | Supported CPUs / instruction sets |
|---|---|---|---|
| macOS | `CoherencyLatency-macOS-v1.1.0.zip` | Intel Mac: macOS 10.7 Lion (2011); Apple Silicon Mac: macOS 11.0 Big Sur (2020) | A single Universal 2 executable containing both x86_64 and arm64 slices (covers M1/M2/M3/M4); no need to pick a version |
| Windows | `CoherencyLatency-Windows-v1.1.0.exe` | x86 / x64: Windows Vista SP2 (2009); ARM64: Windows 10 | A single-file launcher that auto-matches native x86, x64 and ARM64 payloads; the x86 build targets the plain 32-bit baseline (SSE2 not even required), x64/ARM64 use their base instruction sets |
| Android (CLI) | `CoherencyLatency-Android-v1.1.0.zip` | Android 4.1 (API 16) | Four standalone executables: arm64-v8a / armv7 / x86 / x86_64; run from Termux or `adb shell` |
| Android (GUI) | `CoherencyLatency-Android-UI-v1.1.0.apk` | Android 4.1 (API 16) | One APK containing arm64-v8a, armv7, x86 and x86_64 native libraries; works on phones, tablets, round watches and Android TV |
| HarmonyOS | `CoherencyLatency-HarmonyOS-v1.2.6.zip` (unsigned HAP) | HarmonyOS 5.0.0 (API 12) | arm64-v8a devices / x86_64 emulator; phones, tablets and 2-in-1; detects 2–20 logical threads at runtime |

Notes:

- **No instruction-set requirements.** The source is plain C++17 with no SSE/AVX/NEON intrinsics; atomics compile to each architecture's native atomic instructions, so any x86, x64 or ARM64 CPU works.
- **No runtimes to install.** The Windows build links the CRT statically; the macOS build depends only on the system-provided `/usr/lib/libSystem.B.dylib`.
- **How old systems are supported.** The macOS build avoids newer C++ runtime symbols, resolves the 10.10+ QoS API at runtime via `dlsym` (skipped on older systems), and links `SUBSYSTEM 6.0` with a static CRT on Windows.
- **On ARM64 Windows** the launcher itself (x86) runs through the built-in emulation layer, while the embedded ARM64 payload executes natively.
- **Architecture-adaptive, no version picking.** One file covers two macOS architectures; one file covers three Windows architectures. `--version` and `--list` confirm the program starts on your machine.
- **HarmonyOS must be signed locally.** The release contains an unsigned HAP and full source; debug certificates/profiles are bound to device UDIDs and cannot be reused. See [HarmonyOS README](harmonyos/README.md).
- **HarmonyOS compatibility boundary.** Verified on AL00U (Kirin 9000SL, 9 threads / 6 physical cores, HarmonyOS 5.0.0); API 12+ arm64 devices can usually install it, but whether the vendor scheduler allows pinning must be checked on each device — pairs that cannot be pinned are reported as `N/A`. HarmonyOS 4.x, 32-bit ABIs and unverified future devices/SoCs are not covered.

> This is an unofficial port and is not affiliated with, or endorsed by, CapFrameX or CXWorld.

`CoherencyLatency` measures how long a cache line takes to move between two logical CPUs. Two worker threads repeatedly perform an atomic compare-and-swap on the same cache line (`bounce`) or write their own cache lines and read the other thread's line (`owned`). Every ordered pair of logical CPUs is measured, and the result is written as a semicolon-separated matrix.

## Download

Latest releases: [v1.1.0](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/tag/v1.1.0) for macOS / Windows / Android, and [v1.2.6](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/tag/v1.2.6) for HarmonyOS:

| Platform | Release asset |
|---|---|
| macOS | [CoherencyLatency-macOS-v1.1.0.zip](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/download/v1.1.0/CoherencyLatency-macOS-v1.1.0.zip) |
| Windows | [CoherencyLatency-Windows-v1.1.0.exe](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/download/v1.1.0/CoherencyLatency-Windows-v1.1.0.exe) |
| Android (CLI) | [CoherencyLatency-Android-v1.1.0.zip](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/download/v1.1.0/CoherencyLatency-Android-v1.1.0.zip) |
| Android (GUI) | [CoherencyLatency-Android-UI-v1.1.0.apk](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/download/v1.1.0/CoherencyLatency-Android-UI-v1.1.0.apk) |
| HarmonyOS | [CoherencyLatency-HarmonyOS-v1.2.6.zip](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/download/v1.2.6/CoherencyLatency-HarmonyOS-v1.2.6.zip) |

The macOS download is a zip containing a native executable. If Gatekeeper blocks the first launch, right-click the file and choose **Open**, or remove the quarantine attribute with `xattr -dr com.apple.quarantine CoherencyLatency`.

The Windows download is a single `.exe`: it runs the native program matching the current Windows installation and deletes the temporary extracted file when it exits. No installer, nothing written to the registry, static CRT, and no Visual C++ runtime required. It runs on Windows Vista SP2 / 7 / 8 / 8.1 / 10 / 11. The first launch of a freshly downloaded file may take up to a few tens of seconds while Windows Defender scans it.

The Android CLI build is a zip with four ABI executables for Termux or `adb shell`; the GUI build is a single APK for phones, tablets, round watches and Android TV. Both support Android 4.1 (API 16) or later.

The HarmonyOS download is a zip containing an unsigned HAP, full source, the manual and a signing guide. The unsigned HAP cannot be installed directly; sign it with DevEco Studio or `hap-sign-tool` first.

Chinese documentation is available for all platforms: [macOS 使用手册](docs/使用手册.md) ([PDF](docs/使用手册.pdf)), [Windows 使用说明](docs/Windows-使用说明.md), [Android CLI 使用手册](android/docs/使用手册.md) ([PDF](android/docs/使用手册.pdf)), [Android APK 使用说明](android/docs/APK使用说明.md) ([PDF](android/docs/APK使用说明.pdf)) and [HarmonyOS 使用手册](harmonyos/docs/使用手册.md) ([PDF](harmonyos/docs/使用手册.pdf)).

## Relationship to the original

This is a port, not a small patch. The benchmark idea, the `bounce` / `owned` modes, the all-to-all matrix and the semicolon-separated CSV format come from [MicroBenchX](https://github.com/CXWorld/MicroBenchX) by CapFrameX (MIT); the threading, affinity, timing and packaging layers were rewritten.

| | [MicroBenchX CoherencyLatency](https://github.com/CXWorld/MicroBenchX) | CoherencyLatency-MultiPlatform (this repository) |
|---|---|---|
| Platforms | Windows (official v1.0.5 release) | macOS, Windows, Android, HarmonyOS |
| Timing | `QueryPerformanceCounter` | `mach_absolute_time` on macOS, `QueryPerformanceCounter` on Windows, `clock_gettime(CLOCK_MONOTONIC)` on Android and HarmonyOS |
| CPU affinity | Windows thread affinity | `SetThreadAffinityMask` plus a `GetCurrentProcessorNumber` check on Windows; `sched_setaffinity` plus a `sched_getcpu` check on Android and HarmonyOS, with failed pins reported as `N/A`; best-effort QoS hint on macOS |
| Threads | Two new threads per run | Two threads per pair, reused across runs |
| Matrix memory | O(n² × runs) sample buffer | O(n²) matrix, online minimum, stack-allocated cache-line cells |
| Build | Visual Studio solution | CMake for macOS, MSVC batch scripts for Windows, NDK r21e clang scripts for Android, DevEco Studio / hvigor for HarmonyOS |

## What this port adds

**Native macOS build.** The timing layer uses `mach_absolute_time`, which works back to macOS 10.7, instead of newer `std::chrono` symbols. The released executable supports both older and newer Macs.

**Windows single-file launcher.** The launcher embeds the Windows native program as a resource. It extracts the program matching the current Windows installation to a temporary file, runs it, and removes it on exit. Releases are therefore a single `.exe`.

**Native Android port.** The CLI build is cross-compiled with NDK r21e into four ABIs (arm64-v8a, armv7, x86, x86_64), with 32-bit builds supporting Android 4.1 (API 16) and later. The GUI build packs the same four native libraries into one APK that adapts to phones, tablets, round watches and Android TV. The Android build also pins with `sched_setaffinity` and verifies placement with `sched_getcpu`; pairs that fail to pin are reported as `N/A`.

**Native HarmonyOS port.** The measurement core is a C++ NAPI module and the UI is ArkTS. At startup it probes pinnable logical threads and builds a dynamic N×N matrix (2–20 threads), reads SMT, cluster and top-performance cores from sysfs without a model whitelist, and reports `N/A` for pairs that cannot be pinned or migrate during measurement. The release is an unsigned HAP plus source; sign it locally before installing.

**Fixed minimum statistic.** The original source contains a stray semicolon in the minimum-selection loop, which makes the reported result depend on the last run rather than the fastest one. This port takes the minimum over every run, as intended.

**Lower runtime overhead.** Each core pair creates two threads once and reuses them for every run through a per-round start gate, so thread creation never enters the measurement; timing uses an integer high-resolution clock (`QueryPerformanceCounter` on Windows, `mach_absolute_time` on macOS) instead of millisecond-resolution `ftime`. With 8 slots and 10 runs a sweep takes 54 ms instead of 139 ms; `--cpus 32` takes 1612 ms instead of 2914 ms.

**Lower memory use.** The matrix is stored as one O(n²) float array. Each pair's minimum is updated online while runs execute, so no O(n² × runs) sample buffer is allocated; the two cache-line cells used by a pair live on the stack. At 32 slots and 50 runs the peak working set is 4292 KB versus 4428 KB for the original, which keeps growing with `--runs`.


**Alignment of measured numbers with the original.** The optimisations reduce sweep time and memory use only; they do not change the way per-operation latency is measured. In same-machine interleaved comparisons against a rebuild of the original source (4 slots, 1,000,000 iterations per run, 10 runs), the matrices differ by 0.1–2.4 ns and vary between sessions — within environmental noise (thread migration between processor groups can drift readings by ±2–3 ns). The small residual differences come from two deliberate corrections: the statistic is the minimum over all runs (the original accidentally reports the last run due to a stray semicolon), and timing uses high-resolution clocks (the original uses millisecond-resolution `ftime`). Output is therefore expected to agree with the original only to within a few nanoseconds of jitter; this is measurement noise, not a systematic bias.

**Preserved command line and output.** The original modes, option names, all-to-all matrix and semicolon-separated CSV output are unchanged. Existing scripts can keep using the same arguments and the default `core_latencies_<time>.csv` file name.

**macOS scheduling caveat.** macOS does not allow hard CPU pinning. The port asks for a best-effort affinity/QoS hint, but the scheduler can still move threads between performance and efficiency cores. Matrix indices are logical slots, not verified physical core IDs.

**Unpinned pairs are reported as N/A.** On Windows each worker verifies its placement with `GetCurrentProcessorNumber()` after `SetThreadAffinityMask`. If the system rejects the mask (processor groups, virtual machines, containers, cores taken by another process) or the thread does not land on the requested processor, that pair's cells are written as `N/A` and a warning is printed after the matrix, instead of reporting a number measured on the wrong core. The CSV contains `N/A` in the same cells. macOS has no hard pinning, so the macOS build never emits `N/A`.

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
- For Android: NDK r21e, Android SDK build-tools 31.0.0, platforms/android-31 and JDK 17
- For HarmonyOS: DevEco Studio 5.0 or newer with HarmonyOS SDK API 12+ (the project uses compatibleSdkVersion 5.0.0(12))

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

### Android

CLI build (four ABIs):

```bash
cd android
python build_cli.py
python make-android-zip.py
```

GUI APK (the same four ABIs in one APK):

```bash
cd android/app
python build_apk.py
```

`build_cli.py` defaults to `D:\ndk21\android-ndk-r21e`; `build_apk.py` defaults to the JDK 17 and Android SDK under `D:/AndroidTools`. Override with `JAVA_HOME`, `ANDROID_SDK_ROOT` and `ANDROID_NDK_ROOT`.

### HarmonyOS

Open `harmonyos/` in DevEco Studio, change `bundleName` in `AppScope/app.json5`, configure signing and build. Command-line build:

```bash
export DEVECO_SDK_HOME="/path/to/DevEco Studio/sdk"
node "/path/to/DevEco Studio/tools/hvigor/bin/hvigorw.js" \
  --mode module -p product=default assembleHap --no-daemon
```

The output is `harmonyos/entry/build/default/outputs/default/entry-default-unsigned.hap`. The repository contains no signing certificate; see [harmonyos/README.md](harmonyos/README.md) for signing and installation.

## Third-party components

None. The port uses only the C++17 standard library and the platform APIs (`pthread` and Mach on macOS, Win32 on Windows, pthread / libc / QoS on HarmonyOS). No external libraries need to be installed to build or run it.

## Licence

MIT — see [LICENSE](LICENSE).

Based on [MicroBenchX](https://github.com/CXWorld/MicroBenchX) by CapFrameX, licensed under the MIT Licence. The original copyright notice `Copyright (c) 2021 CapFrameX` is preserved in `LICENSE`.

This repository covers `CoherencyLatency` only; `MicroBenchX.Ipc` and the other MicroBenchX tools are out of scope.
