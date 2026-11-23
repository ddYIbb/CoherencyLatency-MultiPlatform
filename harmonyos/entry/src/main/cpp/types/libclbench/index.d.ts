/** CoherencyLatency 原生测量核心 */
export const version: () => string;
/** "逻辑核心数|缓存行字节|ABI" */
export const deviceInfo: () => string;
/**
 * 返回 0 表示已启动；1 已在运行；2 核心/线程列表非法；3 迭代次数非法；4 轮数非法；6 线程创建失败。
 * threadMode 为 true 时 coreIds 是逻辑线程号，矩阵测量线程之间的延迟。
 */
export const start: (coreIds: number[], iterations: number, runs: number, owned: boolean,
  threadMode: boolean) => number;
/** 轮询进度：state 0 空闲 / 1 运行中 / 2 完成 / 3 已停止 / 4 失败 */
export interface PollResult {
  state: number;
  done: number;
  total: number;
  row: number;
  column: number;
  latency: number;
  threadDone: number;
  threadTotal: number;
  running: boolean;
  error: string;
}
export const poll: () => PollResult;
export const matrix: () => number[];
export const csv: () => string;
/** 同核 SMT 线程对："first,second,value;..."，值为 ns/op，失败为 N/A */
export const threadPairs: () => string;
export const abort: () => void;
/** 探测本机真正允许绑定的 CPU 列表（厂商调度器会静默改写部分核心的掩码） */
export const probeCores: () => number[];
/** 返回全部在线逻辑线程号（0..N-1），供线程延迟矩阵使用 */
export const probeThreads: () => number[];
/** 探测到的 CPU 拓扑："CPU:物理核;..."，同一物理核出现两次即为超线程兄弟 */
export const coreTopology: () => string;
