# CoherencyLatency Windows 使用说明

**版本：** 1.0.0（对应 MicroBenchX 1.0.5）  
**适用平台：** Windows Vista SP2 / 7 / 8 / 8.1 / 10 / 11；ARM64 版需要 Windows 10/11 on ARM64。

## 1. 文件说明

Release 中的 `CoherencyLatency-Windows-x86-x64-ARM64-v1.0.0.exe` 是一个单文件启动器。它内部嵌入了 x86、x64 和 ARM64 三份原生程序，启动时自动识别当前 CPU，释放匹配的版本到临时目录运行，退出后自动删除临时文件。

- 不需要安装，不写注册表。
- 三份 payload 都静态链接 CRT，不需要另行安装 Visual C++ 运行库。
- x86 版可以在 64 位 Windows 上通过 WOW64 运行。
- ARM64 版只能在 Windows on ARM 上原生运行；在 x64 机器上会自动选择 x64 版。

## 2. 首次运行

1. 把 exe 复制到一个可写的目录，例如 `D:\Tools\CoherencyLatency\`。
2. 双击运行，或在“命令提示符 / PowerShell”中执行：

~~~bat
CoherencyLatency-Windows-x86-x64-ARM64-v1.0.0.exe --list
~~~

`--list` 只打印 CPU 信息，不运行测试。

首次运行新下载的 exe 时，Windows Defender 或 SmartScreen 可能会扫描文件，启动会慢几秒到几十秒；之后启动会恢复正常。如果 SmartScreen 拦截，请点击“更多信息 → 仍要运行”。

## 3. 命令行

| 参数 | 说明 |
|---|---|
| `-i, --iterations N` | 每个线程成功完成锁操作的次数，默认 1000000。 |
| `-r, --runs N` | 每个有序 CPU 对的采样次数，默认 10。 |
| `-m, --mode MODE` | `bounce`（同一缓存行，默认）或 `owned`（各自独立的缓存行）。 |
| `--cpus N` | 最多使用 N 个逻辑槽位，默认使用全部逻辑 CPU。 |
| `--perf-only` | 在 Apple Silicon 上限制逻辑槽位；Windows 版无影响。 |
| `--csv PATH` | CSV 输出路径，默认在当前目录生成 `core_latencies_<时间>.csv`。 |
| `-y, --yes` | 测试前后不等待回车。 |
| `--no-progress` | 关闭进度条。 |
| `--no-affinity` | 关闭线程亲和性设置。 |
| `--list` | 打印 CPU 信息后退出。 |
| `-h, --help` | 显示帮助。 |
| `--version` | 显示版本。 |

示例：

~~~bat
CoherencyLatency-Windows-x86-x64-ARM64-v1.0.0.exe --cpus 8 --iterations 1000000 --runs 10 --mode bounce --yes
~~~

也可以使用原版的位置参数写法：

~~~bat
CoherencyLatency-Windows-x86-x64-ARM64-v1.0.0.exe 1000000 bounce
~~~

## 4. CSV 输出

默认文件名为 `core_latencies_YYYYMMDD_HHMMSS.csv`，使用分号 `;` 分隔：

- 第一行：`;Core 0;Core 1;...`
- 每一行：`Core i;值;值;...`
- 对角线为 `x`
- 数值按原版 `defaultfloat` 精度 6 输出

原版输出的是 **CSV，不是 XLSX**。可以直接用 Excel 或 Numbers 打开，也可以在 Excel 中另存为 `.xlsx`；程序本身不生成 xlsx。

## 5. 结果解读与限制

- 矩阵下标是逻辑处理器编号，不一定等于物理核心编号。
- Windows 版使用 `SetThreadAffinityMask` 把线程绑定到指定逻辑处理器；如果系统或虚拟机限制了亲和性，结果可能不稳定。
- 测量时建议关闭其他负载、连接电源，并避免热降频。
- 不要把 Windows 与 macOS 的数值直接比较，平台和调度模型不同。

## 6. 源码构建

源码和构建脚本说明见 [windows/README.md](../windows/README.md)。三个 payload 构建完成后，在仓库根目录构建单文件启动器：

~~~bash
cmake -S tools/launcher -B tools/launcher/build -A Win32
cmake --build tools/launcher/build --config Release
~~~

如果 CMake 的 Visual Studio 生成器找不到编译器，可以在 Visual Studio 开发者命令提示符中改用 Ninja：

~~~bash
cmake -S tools/launcher -B tools/launcher/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools/launcher/build
~~~

生成的 `tools/launcher/build/CoherencyLatency.exe` 就是单文件启动器。