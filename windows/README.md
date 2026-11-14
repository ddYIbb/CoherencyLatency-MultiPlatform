# Windows 构建说明

本目录包含 `CoherencyLatency` 的 Windows 原生 payload 源码。单文件启动器在 [`tools/launcher`](../tools/launcher/CMakeLists.txt)。

## 构建三个原生 payload

在 `windows\src` 目录打开命令行，确保已安装 Visual Studio 的 C++ 桌面开发工具，然后运行：

```bat
build.bat x86
build.bat x64
build.bat arm64
```

脚本使用 `/MT` 静态链接 CRT，目标最低系统为 Windows Vista SP2；输出分别生成到：

- `windows/x86/CoherencyLatency.exe`
- `windows/x64/CoherencyLatency.exe`
- `windows/ARM64/CoherencyLatency.exe`

如果 Visual Studio 不在默认路径，请设置 `VSINSTALL` 环境变量后重试。

## 构建单文件启动器

三个 payload 构建完成后，在仓库根目录运行：

```bash
cmake -S tools/launcher -B tools/launcher/build -A Win32
cmake --build tools/launcher/build --config Release
```

如果 CMake 的 Visual Studio 生成器找不到编译器，可以在 Visual Studio 开发者命令提示符中改用 Ninja：

```bash
cmake -S tools/launcher -B tools/launcher/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools/launcher/build
```

生成的 `tools/launcher/build/CoherencyLatency.exe` 会按当前 Windows 架构自动选择 payload，运行结束后删除临时文件。

## 系统要求

- x86 / x64：Windows Vista SP2 / 7 / 8 / 8.1 / 10 / 11
- ARM64：Windows 10 / 11 ARM64
- 不支持 Windows XP

首次运行新下载的 exe 时，Windows Defender 可能需要几十秒扫描文件；之后启动通常很快。
