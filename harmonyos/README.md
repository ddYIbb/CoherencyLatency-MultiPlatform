# CoherencyLatency-HarmonyOS

**状态：** 已完成移植并在 HarmonyOS 真机（nova 12 Ultra / ADA-AL10U，HarmonyOS 7.0.0.111 / API 26）跑通，版本 1.2.0；发布包为未签名 HAP。

把 MicroBenchX v1.0.5 的 `CoherencyLatency`（CPU 核间缓存一致性延迟测试）移植为鸿蒙原生应用：
上层 ArkTS + ArkUI，下层 C++ 测量核心通过 NAPI 暴露给 ArkTS。

## 功能

- 启动时探测在线逻辑线程，按实际线程数构建 N×N 矩阵（支持 2–20 个线程）；
- 从 sysfs 动态读取 `thread_siblings`、`physical_package_id`、`cpu_capacity` / 最高频率，
  自动识别 SMT 兄弟、簇与最高性能核，**不使用机型白名单**；
- 一次配对运行已经包含往返两个方向并除以 2，矩阵只测上三角再对称填充，避免重复测量；
- 实时显示进度与结果矩阵（Bounce 模式）；
- 支持中途停止，已测结果保留；
- 完成后自动保存 CSV 到应用沙箱，支持复制到剪贴板 / 另存；
- 绑定失败的线程对显示 `N/A`，不伪造结果；
- 不联网、不申请权限。

## 目录结构

~~~text
CoherencyLatency-HarmonyOS/
├── AppScope/                       应用级配置（versionName 1.2.0 / versionCode 1000207）
├── entry/src/main/
│   ├── cpp/cl_bench.cpp            C++ 测量核心 + NAPI 绑定
│   ├── cpp/types/libclbench/       NAPI 类型声明（index.d.ts）
│   ├── ets/pages/Index.ets         ArkTS 主界面
│   ├── ets/entryability/           EntryAbility
│   └── module.json5                deviceTypes: phone / tablet / 2in1
├── build-profile.json5             compatibleSdkVersion 5.0.0(12)、targetSdkVersion 26.0.0
├── signing-workspace/              签名材料与说明
└── reference-src/                  移植参考源码
~~~

## 构建

~~~bash
cd /d/Users/Administrator/Music/MicroBenchX_v1.0.5/CoherencyLatency-HarmonyOS
export DEVECO_SDK_HOME='D:/Program Files/Huawei/DevEco Studio/sdk'
"D:/Program Files/Huawei/DevEco Studio/tools/node/node.exe" \
  "D:/Program Files/Huawei/DevEco Studio/tools/hvigor/bin/hvigorw.js" \
  --mode module -p product=default assembleHap --no-daemon
~~~

产物：`entry/build/default/outputs/default/entry-default-signed.hap`

## 安装与运行

~~~bash
hdc install -r entry-default-signed.hap
hdc shell aa start -a EntryAbility -b com.example.coherencylatency
~~~

## 动态拓扑与性能处理（1.2.0）

- **动态拓扑**：在线 CPU 列表、SMT 兄弟、簇、最高性能核全部来自 sysfs；矩阵尺寸随逻辑线程数变化，
  当前支持 2–20 线程，界面按 20 列设计并允许横向滚动；
- **最高性能核判定**：按单核 `cpu_capacity`（缺失时按最高频率）取全局最高档，只把这些物理核当 prime，
  避免同一 package 内的中核被误标；
- **上三角测量**：N 个线程只测 N×(N-1)/2 个无向线程对，每对结果同时填入对称两格；
- **采样**：线程矩阵每轮 500 次原子操作、每方向 800 轮，每对至少保留 64 轮有效样本，样本不足自动重试；
- **慢档逃逸**：结果高于同核延迟的 5 倍（最低 80 ns，缺省 135 ns）时换反向窗口重测，最多 16 个窗口取最小值；
- **绑核重试**：最高性能核 40 轮、普通跨核 8 轮、普通同核 20 轮；最高性能核交叉对在常规重试后
  再用精确单线程掩码回退最多 24 次（间隔 20 ms），仍失败才记 N/A；
- **绑核窗口处理**：失败线程对在窗口打开后按 120 ms 步长轮换三种请求形状（group / 锁 A / 锁 B）重试，恢复扫描同样轮换掩码；
  测试期间界面持续出帧，避免系统把应用判为空闲窗口而收回最高性能核；
  prime 判定按“单核性能档”，矩阵开始前不再做第二次全量探测；
- 每个线程的真实落点由 `/proc/thread-self/stat` 的 processor 字段判定，测量期间由 monitor 持续核对，
  落点不符整对重试；
- 本机冷启动、不触控最终验收连续 10 次 72/72、0 N/A，用时 21–112 s（波动来自等绑核窗口）；
  代表轮 39.34 s，平均 97.1 / 最小 25.0 / 最大 120.3 ns；
- 内存：冷启动 PSS 约 42 MB，跑完一次约 58 MB（主要是结果矩阵的 ArkUI/GL 渲染），多次运行未发现泄漏。

## 已知平台限制

部分鸿蒙机型由厂商调度器接管 CPU 亲和性：应用对某些核心调用 `sched_setaffinity` 时函数可能返回成功，
但内核实际把线程放到别的核。应用会逐线程探测落点、测量时持续核对，并对最高性能核交叉对做精确回退；
实在绑不住的线程对显示 N/A，不会用错误核心的数据冒充结果。

本机示例为 9 个逻辑 CPU、6 个物理核（CPU 3/4、CPU 5/6、CPU 7/8 为超线程对）。换一台设备，
线程数、SMT 标签、簇、最高性能核与矩阵大小都会自动变化；系统负载或温度变化时仍可能有个别 N/A。

## 发布包

已打包到 `../CoherencyLatency-HarmonyOS-Release/`（未签名 HAP + 自行签名说明 + 使用手册 md/html/pdf + README + SHA256SUMS + 可构建源码），
压缩包为 `../CoherencyLatency-HarmonyOS-v1.2.0.zip`。

## 许可

MIT，基于 [CapFrameX/MicroBenchX](https://github.com/CXWorld/MicroBenchX) v1.0.5（Copyright (c) 2021 CapFrameX）。
