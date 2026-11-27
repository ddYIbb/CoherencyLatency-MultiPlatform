# CoherencyLatency HarmonyOS 版

**状态：** 已在 HarmonyOS 真机（nova 12 Ultra 星耀版 / ADA-AL10U，HarmonyOS 7.0.0.111 / API 26）跑通，版本 1.3.0；并在手机 / 折叠屏 / 阔折叠 / 三折叠 / 平板 / 2in1 / 电视 / 圆形手表 / 方形手表 / 车机共 11 类设备上完成形态适配验证。  
**发布包：** [v1.3.0](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/tag/v1.3.0)（未签名 HAP + 完整源码 + 使用手册 + 自行签名说明）。  
**许可：** MIT，基于 CapFrameX 的 [MicroBenchX v1.0.5](https://github.com/CXWorld/MicroBenchX)。

本目录是 `CoherencyLatency` 的鸿蒙原生移植：测量核心用 C++ 写成 NAPI 原生模块，界面用 ArkTS 编写。它测量一条缓存行在两个逻辑线程之间往返一次需要多少纳秒（ns），输出 N×N 线程对矩阵和分号分隔的 CSV。

## 1. 功能与设计

- 启动时探测在线逻辑线程，按实际可绑定线程数构建 N×N 矩阵（支持 2–20 个线程）。
- 从 sysfs 动态读取 `thread_siblings`、`physical_package_id`、`cpu_capacity` / 最高频率，自动识别 SMT 兄弟、簇与最高性能核。
- **没有机型白名单**，也不按 CPU 编号写死拓扑；换处理器、换簇排列时矩阵与判定自动变化。
- 测 N×(N-1) 个有序线程对：`[i][j]` 与 `[j][i]` 分别测量、分别记录，不取平均、不互相复制，所以上下三角可以不同。
- 测量过程中实时显示进度与矩阵，随时可停止，已测结果保留；完成后自动保存 CSV。
- 绑定失败或测量期间线程被迁移的线程对显示 `N/A`，不会拿错误核心的数据冒充结果。
- 不联网、不申请权限、不需要 root。

## 2. 兼容性与验证边界

| 层次 | 内容 |
|---|---|
| 已验证 | ADA-AL10U（nova 12 Ultra 星耀版，Kirin 9000SL，9 线程 / 6 物理核 / 3 簇，HarmonyOS 7.0.0.111 / API 26）：1.2.0 版验收连续 10 次 72/72、0 N/A，用时 21–112 s；截图为 39.34 s，平均 97.1 / 最小 25.0 / 最大 120.3 ns；另在 AL00U 同规格真机上完成开发期验证 |
| 设计支持 | HarmonyOS 5.0.0（API 12）及以上；phone / tablet / 2in1 / tv / wearable / car；arm64-v8a 真机、x86_64 模拟器；2–20 线程；有/无 SMT、单簇/多簇、大小核/多簇均由 sysfs 实测适配 |
| 形态验证 | 直板手机（nova 12 Ultra / AL10U、AL00U）、折叠屏（Mate X7）、阔折叠（Pura X Max）、三折叠（Huawei TripleFold，9 种折叠形态）、平板（MatePad Pro 13）、2in1 折叠（Huawei 2in1 Foldable，half-open/open/close）、电视（Huawei TV，遥控器 D-pad 焦点）、圆形手表（Huawei Wearable 466×466）、方形手表（Huawei Wearable Kids 480×408）、车机（Huawei Car 3402×1620）：均完成 12/12 或 72/72 实测 |
| 未验证 | HarmonyOS 4.x 及更早；32 位 ABI；未逐台实测的后续机型与新 SoC 的调度行为 |

工程 `compatibleSdkVersion` 为 `5.0.0(12)`，`targetSdkVersion` 为 `26.0.0`。**“设计支持”不等于“所有机型都已验证”**：同一 HAP 通常能在 API 12+ 的 arm64 设备上安装运行，但厂商调度器是否允许绑核、允许绑哪些核必须实机确认；程序会探测真实落点，绑不住就显示 `N/A`。

本目录只包含 HarmonyOS 工程。Windows、macOS、Android 是各自独立编译的移植版，不能安装此 HAP；它们与鸿蒙版共用测量原则，但不是同一个二进制。

## 3. 测量参数

- **采样：** 每轮 500 次原子操作，每方向 800 轮，每对至少保留 64 轮有效样本。
- **慢档逃逸：** 高于同核延迟的 5 倍（最低 80 ns，缺省 135 ns）时换反向窗口重测，最多 16 个窗口取最小。
- **绑核重试：** 最高性能核 40 轮、普通跨核 8 轮、普通同核 20 轮；最高性能核交叉对再用精确单线程掩码回退最多 24 次（间隔 20 ms）。
- **UI 刷新：** 原生侧每完成一个线程对才递增 `done`，界面只在 `done` 变化时刷新一次，用时不会每 120 ms 跳动。

9×9 通常约 25–115 秒（含等待绑核窗口，状态好时 20–40 秒）；20×20 需要数分钟。线程越多、绑核重试越多，用时越长。

## 4. 构建

环境：

- DevEco Studio 5.0 或更新版本，安装 HarmonyOS SDK（API 12+）。
- 工程不依赖第三方库；C++ 侧只使用标准库、pthread、libc 与 HarmonyOS QoS API。

用 DevEco Studio 打开本目录，修改 `AppScope/app.json5` 里的 `bundleName`，配置签名后直接 Build。命令行构建示例（把路径替换成你的 DevEco Studio 安装位置）：

```bash
export DEVECO_SDK_HOME="/path/to/DevEco Studio/sdk"
node "/path/to/DevEco Studio/tools/hvigor/bin/hvigorw.js" \
  --mode module -p product=default assembleHap --no-daemon
```

产物为 `entry/build/default/outputs/default/entry-default-unsigned.hap`。工程里的 `build-profile.json5` 刻意不包含任何签名配置，避免把本机证书路径或密码带进仓库；请用 DevEco Studio 的 `Signing Configs` 或 `hap-sign-tool` 自行签名。

## 5. 签名与安装

发布包提供的是**未签名 HAP**，`hdc install` 会报签名校验失败。调试证书/Profile 只允许安装到创建 Profile 时登记的设备的 UDID，换设备要重新生成，因此仓库不提供带签名的 HAP。

最短路径：

1. 用 DevEco Studio 打开本目录，修改 `bundleName`；
2. File → Project Structure → Signing Configs → 勾选 Automatically generate signature，连接设备；
3. Build → Build Hap(s)/APP(s) → Build Hap(s)；
4. 安装生成的 `entry-default-signed.hap`：

```bash
hdc install -r entry-default-signed.hap
hdc shell aa start -a EntryAbility -b com.example.coherencylatency
```

要上架应用市场，请使用发布证书和发布 Profile，并按华为要求提交审核。完整步骤见发布包里的《自行签名说明.md》。

## 6. 文档与截图

- [使用手册](docs/使用手册.md)（[PDF](docs/使用手册.pdf)）
- [主界面截图](docs/screenshots/主界面.jpg)
- [测试结果截图](docs/screenshots/测试结果.jpg)
- 发布包：[v1.3.0](https://github.com/ddYIbb/CoherencyLatency-MultiPlatform/releases/tag/v1.3.0)

## 7. 许可

MIT，详见仓库根目录 [LICENSE](../LICENSE)。本项目基于 CapFrameX 的 [MicroBenchX](https://github.com/CXWorld/MicroBenchX)（MIT 许可）修改而来，原版权声明 `Copyright (c) 2021 CapFrameX` 保留在 LICENSE 中。

非官方移植项目，与 CapFrameX / CXWorld 没有隶属或背书关系。
