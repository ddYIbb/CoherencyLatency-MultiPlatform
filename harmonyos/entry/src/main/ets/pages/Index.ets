import { pasteboard } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import {
  version as benchVersion,
  deviceInfo as benchDeviceInfo,
  start as benchStart,
  poll as benchPoll,
  matrix as benchMatrix,
  csv as benchCsv,
  abort as benchAbort,
  probeThreads as benchProbeThreads,
  coreTopology as benchCoreTopology,
  PollResult
} from 'libclbench.so';

const DEFAULT_ITERATIONS: number = 1000000;
const DEFAULT_RUNS: number = 3;
const POLL_INTERVAL_MS: number = 120;

const BACKGROUND: string = '#F4F6F9';
const CARD: string = '#FFFFFF';
const TEXT_PRIMARY: string = '#101828';
const TEXT_SECONDARY: string = '#667085';
const TEXT_TERTIARY: string = '#98A2B3';
const ACCENT: string = '#2563EB';
const ACCENT_DARK: string = '#1E40AF';
const ACCENT_BORDER: string = '#BFD3FE';
const BORDER: string = '#E4E7EC';
const STOP_BACKGROUND: string = '#F2F4F7';

const STATE_IDLE: number = 0;
const STATE_RUNNING: number = 1;
const STATE_FINISHED: number = 2;
const STATE_ABORTED: number = 3;
const STATE_FAILED: number = 4;

@Entry
@Component
struct Index {
  @State private logicalCores: number = 2;
  @State private coreIds: number[] = [];
  @State private cacheLine: number = 128;
  @State private abi: string = '';
  @State private n: number = 2;
  /** -1 未测，NaN 表示绑核失败 */
  @State private cells: number[] = [];
  @State private running: boolean = false;
  @State private donePairs: number = 0;
  @State private totalPairs: number = 0;
  @State private statusText: string = '就绪';
  @State private caption: string = '线程延迟矩阵 · ns/op';
  @State private savedMessage: string = ' ';
  @State private hasResult: boolean = false;
  @State private versionText: string = '';
  @State private physicalCount: number = 0;
  @State private smtText: string = '';
  @State private hasSmt: boolean = false;

  private timerId: number = -1;
  private startMillis: number = 0;
  private lastCsv: string = '';
  private lastCsvName: string = '';
  private physicalKeys: number[] = [];
  private smtFlags: boolean[] = [];

  aboutToAppear(): void {
    this.readDevice();
    this.versionText = benchVersion();
  }

  aboutToDisappear(): void {
    this.stopTimer();
    if (this.running) {
      benchAbort();
    }
  }

  onPageHide(): void {
    if (this.running) {
      benchAbort();
    }
    this.stopTimer();
  }

  // -------------------------------------------------------------------------
  // 设备信息与矩阵初始化
  // -------------------------------------------------------------------------

  private readDevice(): void {
    let cores: number = 2;
    let line: number = 128;
    let abi: string = 'arm64-v8a';
    const raw: string = benchDeviceInfo();
    if (raw.length > 0) {
      const parts: string[] = raw.split('|');
      if (parts.length > 0 && parts[0].length > 0) {
        cores = Number.parseInt(parts[0]);
        if (Number.isNaN(cores) || cores < 1) {
          cores = 2;
        }
      }
      if (parts.length > 1) {
        const parsed: number = Number.parseInt(parts[1]);
        if (!Number.isNaN(parsed)) {
          line = parsed;
        }
      }
      if (parts.length > 2 && parts[2].length > 0) {
        abi = parts[2];
      }
    }
    if (cores > 32) {
      cores = 32;
    }
    this.logicalCores = cores;
    this.cacheLine = line;
    this.abi = abi;

    let threads: number[] = [];
    try {
      threads = benchProbeThreads();
    } catch (error) {
      threads = [];
    }
    this.coreIds = threads;
    this.n = threads.length;
    this.resetCells(this.n);
    this.totalPairs = this.n * (this.n - 1);
    this.parseTopology();
    if (this.n < 2) {
      this.statusText = '本机没有探测到可测试的逻辑线程，无法测试';
    } else {
      this.statusText = '就绪 · ' + this.n + ' 个逻辑线程（' + this.coreLabel() +
        '）· ' + this.n + '×' + this.n + ' 线程矩阵';
    }
  }

  private parseTopology(): void {
    this.physicalKeys = [];
    this.smtFlags = [];
    this.physicalCount = 0;
    this.smtText = '';
    this.hasSmt = false;
    for (let index = 0; index < this.coreIds.length; ++index) {
      this.physicalKeys.push(this.coreIds[index]);
      this.smtFlags.push(false);
    }
    // The native side reports "physical:threads" entries such as "3:3/4";
    // each slash-separated thread is one logical CPU sharing that core.
    const raw: string = benchCoreTopology();
    const distinct: number[] = [];
    if (raw.length > 0) {
      const parts: string[] = raw.split(';');
      for (let index = 0; index < parts.length; ++index) {
        const pair: string[] = parts[index].split(':');
        if (pair.length !== 2) {
          continue;
        }
        const physical: number = Number.parseInt(pair[0]);
        if (Number.isNaN(physical)) {
          continue;
        }
        const threads: string[] = pair[1].split('/');
        let found: boolean = false;
        for (let t = 0; t < threads.length; ++t) {
          const thread: number = Number.parseInt(threads[t]);
          if (Number.isNaN(thread)) {
            continue;
          }
          const at: number = this.coreIds.indexOf(thread);
          if (at < 0) {
            continue;
          }
          found = true;
          this.physicalKeys[at] = physical;
          if (threads.length > 1) {
            this.smtFlags[at] = true;
            this.hasSmt = true;
          }
        }
        if (found) {
          let seen: boolean = false;
          for (let d = 0; d < distinct.length; ++d) {
            if (distinct[d] === physical) {
              seen = true;
              break;
            }
          }
          if (!seen) {
            distinct.push(physical);
          }
          if (threads.length > 1) {
            if (this.smtText.length > 0) {
              this.smtText += '、';
            }
            this.smtText += 'T' + threads.join('/T');
          }
        }
      }
    }
    this.physicalCount = distinct.length;
  }

  private coreLabel(): string {
    const parts: string[] = [];
    for (let index = 0; index < this.coreIds.length; ++index) {
      parts.push('T' + this.coreIds[index]);
    }
    return parts.join(',');
  }

  private coreHeading(index: number): string {
    const thread: number = this.coreIds[index];
    const suffix: string = index >= 0 && index < this.smtFlags.length && this.smtFlags[index] ? '*' : '';
    return 'T' + thread + suffix;
  }

  private matrixLabelWidth(): number {
    if (this.n >= 13) {
      return 22;
    }
    if (this.n >= 10) {
      return 21;
    }
    return this.n >= 9 ? 27 : 40;
  }

  // Horizontal gap between two matrix columns.  The header row must use the
  // very same gap, otherwise every header label drifts half a gap further left
  // than the column it names and the last one is visibly off centre.
  private matrixCellGap(): number {
    if (this.n >= 10) {
      return 0;
    }
    return this.n >= 9 ? 1 : 2;
  }

  // The matrix has to fit the card for every topology, not just the 9-thread
  // Kirin layout.  A four-cluster phone can expose 10-16 logical CPUs; derive
  // the cell width from the available width (about 332 vp inside the card)
  // instead of hard-coding one branch per matrix size.
  private matrixCellWidth(): number {
    const available: number = 332;
    const label: number = this.matrixLabelWidth();
    const gap: number = this.matrixCellGap();
    const computed: number = Math.floor(
      (available - label - (this.n - 1) * gap) / Math.max(1, this.n));
    if (this.n >= 9) {
      // The matrix card scrolls horizontally, so keep cells readable instead
      // of squeezing a 20-thread layout into the card width.
      return Math.max(20, Math.min(32, computed));
    }
    if (this.n === 8) {
      return 33;
    }
    if (this.n === 7) {
      return 37;
    }
    if (this.n >= 6) {
      return 44;
    }
    return 46;
  }

  private matrixFontSize(): number {
    if (this.n >= 17) {
      return 7;
    }
    if (this.n >= 13) {
      return 7.5;
    }
    if (this.n >= 10) {
      return 8;
    }
    if (this.n >= 9) {
      return 8.5;
    }
    if (this.n >= 8) {
      return 9.5;
    }
    if (this.n >= 6) {
      return 11;
    }
    return 11.5;
  }

  private matrixHeaderFontSize(): number {
    if (this.n >= 17) {
      return 7.5;
    }
    if (this.n >= 13) {
      return 8;
    }
    if (this.n >= 10) {
      return 9;
    }
    return this.n >= 9 ? 9.5 : 11;
  }

  private resetCells(cores: number): void {
    const cells: number[] = [];
    for (let index = 0; index < cores * cores; ++index) {
      cells.push(-1);
    }
    this.cells = cells;
  }

  // -------------------------------------------------------------------------
  // 测试生命周期
  // -------------------------------------------------------------------------

  private startTest(): void {
    if (this.running || this.n < 2) {
      return;
    }
    const coreIds: number[] = [];
    for (let index = 0; index < this.coreIds.length; ++index) {
      coreIds.push(this.coreIds[index]);
    }
    this.n = coreIds.length;
    this.resetCells(this.n);
    this.donePairs = 0;
    this.totalPairs = this.n * (this.n - 1);
    this.caption = '实时原始值 · ns/op';
    this.statusText = '准备中…';
    this.hasResult = false;
    this.savedMessage = ' ';
    this.lastCsv = '';
    this.lastCsvName = '';

    const code: number = benchStart(coreIds, DEFAULT_ITERATIONS, DEFAULT_RUNS, false, true);
    if (code !== 0) {
      this.statusText = '测试失败：' + this.describeError(code);
      return;
    }
    this.running = true;
    this.startMillis = Date.now();
    this.startTimer();
  }

  private startTimer(): void {
    this.stopTimer();
    this.timerId = setInterval(() => {
      this.pollOnce();
    }, POLL_INTERVAL_MS);
  }

  private stopTimer(): void {
    if (this.timerId !== -1) {
      clearInterval(this.timerId);
      this.timerId = -1;
    }
  }

  private pollOnce(): void {
    const result: PollResult = benchPoll();
    if (result.state === STATE_RUNNING) {
      // 原生侧每完成一个线程对才递增 done；这里只在出现新结果时更新界面，
      // 避免 120 ms 轮询把"用时"刷成高频跳动的数字（与 Android 版一致）。
      if (result.done <= this.donePairs) {
        return;
      }
      if (result.row >= 0 && result.column >= 0 &&
        result.row < this.n && result.column < this.n) {
        const next: number[] = this.cells.slice();
        next[result.row * this.n + result.column] = Number.isNaN(result.latency) ? Number.NaN : result.latency;
        this.cells = next;
      }
      this.donePairs = result.done;
      this.totalPairs = result.total;
      if (result.row >= 0) {
        const rowLabel: string = 'T' + this.coreIds[result.row];
        const columnLabel: string = 'T' + this.coreIds[result.column];
        if (Number.isNaN(result.latency)) {
          this.statusText = '测量中 ' + result.done + '/' + result.total + ' · ' +
            rowLabel + '↔' + columnLabel + ' N/A（绑核失败）';
        } else {
          this.statusText = '测量中 ' + result.done + '/' + result.total + ' · ' +
            rowLabel + '↔' + columnLabel + ' ' + result.latency.toFixed(1) +
            ' ns · 用时 ' + this.elapsedSeconds().toFixed(1) + ' s';
        }
      }
      return;
    }
    if (result.state === STATE_FINISHED) {
      this.finishTest(result);
      return;
    }
    if (result.state === STATE_ABORTED) {
      this.abortTest();
      return;
    }
    if (result.state === STATE_FAILED) {
      this.failTest(result.error);
    }
  }

  private finishTest(result: PollResult): void {
    this.stopTimer();
    this.running = false;
    const finalMatrix: number[] = benchMatrix();
    if (finalMatrix.length === this.n * this.n) {
      this.cells = finalMatrix;
    }
    this.donePairs = this.totalPairs;
    this.caption = '最终结果 · 双向平均 · ns/op';
    const naPairs: number = this.countNaPairs();
    const elapsed: string = this.elapsedSeconds().toFixed(2);
    let summary: string = '完成 · ' + this.totalPairs + ' 个线程对';
    if (naPairs > 0) {
      summary += ' · ' + naPairs + ' 个 N/A（绑核失败）';
    }
    this.statusText = summary + ' · 用时 ' + elapsed + ' s';
    this.lastCsv = benchCsv();
    this.lastCsvName = 'thread_latencies_' + this.timestamp() + '.csv';
    if (this.lastCsv.length > 0) {
      this.hasResult = true;
      this.autoSave();
    }
  }

  private abortTest(): void {
    this.stopTimer();
    this.running = false;
    this.caption = '已停止 · 部分结果';
    const naPairs: number = this.countNaPairs();
    if (naPairs > 0) {
      this.statusText = '已停止 · 完成 ' + this.donePairs + '/' + this.totalPairs +
        ' 个线程对 · ' + naPairs + ' 个 N/A';
    } else {
      this.statusText = '已停止 · 完成 ' + this.donePairs + '/' + this.totalPairs + ' 个线程对';
    }
    this.savedMessage = ' ';
  }

  private failTest(error: string): void {
    this.stopTimer();
    this.running = false;
    this.caption = '结果矩阵 · ns/op';
    this.statusText = '测试失败：' + (error.length > 0 ? error : '原生错误');
    this.savedMessage = ' ';
  }

  private stopTest(): void {
    if (!this.running) {
      return;
    }
    this.statusText = '正在停止…';
    benchAbort();
  }

  // -------------------------------------------------------------------------
  // 导出
  // -------------------------------------------------------------------------

  private resultDirectory(): string {
    const context = getContext(this) as common.UIAbilityContext;
    return context.filesDir + '/results';
  }

  private autoSave(): void {
    try {
      const directory: string = this.resultDirectory();
      if (!fs.accessSync(directory)) {
        fs.mkdirSync(directory);
      }
      const path: string = directory + '/' + this.lastCsvName;
      const file = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      try {
        fs.writeSync(file.fd, this.lastCsv);
      } finally {
        fs.closeSync(file);
      }
      this.savedMessage = '已自动保存：' + path;
    } catch (error) {
      const failure = error as BusinessError;
      this.savedMessage = '自动保存失败：' + failure.message;
    }
  }

  private copyCsv(): void {
    if (this.lastCsv.length === 0) {
      this.savedMessage = '请先完成一次测试';
      return;
    }
    const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, this.lastCsv);
    pasteboard.getSystemPasteboard().setData(data).then(() => {
      this.savedMessage = 'CSV 已复制到剪贴板（' + this.lastCsvName + '）';
    }).catch((error: BusinessError) => {
      this.savedMessage = '复制失败：' + error.message;
    });
  }

  private exportAgain(): void {
    if (this.lastCsv.length === 0) {
      this.savedMessage = '请先完成一次测试';
      return;
    }
    this.autoSave();
  }

  // -------------------------------------------------------------------------
  // 渲染辅助
  // -------------------------------------------------------------------------

  private countNaPairs(): number {
    let count: number = 0;
    for (let row = 0; row < this.n; ++row) {
      for (let column = 0; column < this.n; ++column) {
        if (row !== column) {
          const value: number = this.cells[row * this.n + column];
          if (Number.isNaN(value)) {
            count += 1;
          }
        }
      }
    }
    return count;
  }


  private measuredValues(): number[] {
    const values: number[] = [];
    for (let row = 0; row < this.n; ++row) {
      for (let column = 0; column < this.n; ++column) {
        if (row !== column) {
          const value: number = this.cells[row * this.n + column];
          if (!Number.isNaN(value) && value > 0) {
            values.push(value);
          }
        }
      }
    }
    return values;
  }

  private minimum(): number {
    const values: number[] = this.measuredValues();
    if (values.length === 0) {
      return 0;
    }
    let min: number = values[0];
    for (let index = 1; index < values.length; ++index) {
      if (values[index] < min) {
        min = values[index];
      }
    }
    return min;
  }

  private maximum(): number {
    const values: number[] = this.measuredValues();
    if (values.length === 0) {
      return 0;
    }
    let max: number = values[0];
    for (let index = 1; index < values.length; ++index) {
      if (values[index] > max) {
        max = values[index];
      }
    }
    return max;
  }

  private average(): number {
    const values: number[] = this.measuredValues();
    if (values.length === 0) {
      return 0;
    }
    let sum: number = 0;
    for (let index = 0; index < values.length; ++index) {
      sum += values[index];
    }
    return sum / values.length;
  }

  private heatColor(value: number): string {
    const min: number = this.minimum();
    const max: number = this.maximum();
    if (max - min < 0.0001) {
      return '#12B76A';
    }
    const ratio: number = Math.max(0, Math.min(1, (value - min) / (max - min)));
    const green: number[] = [18, 183, 106];
    const yellow: number[] = [247, 144, 9];
    const red: number[] = [240, 68, 56];
    const from: number[] = ratio < 0.5 ? green : yellow;
    const to: number[] = ratio < 0.5 ? yellow : red;
    const weight: number = ratio < 0.5 ? ratio * 2 : (ratio - 0.5) * 2;
    const red_channel: number = Math.round(from[0] + (to[0] - from[0]) * weight);
    const green_channel: number = Math.round(from[1] + (to[1] - from[1]) * weight);
    const blue_channel: number = Math.round(from[2] + (to[2] - from[2]) * weight);
    return '#' + this.hexPair(red_channel) + this.hexPair(green_channel) + this.hexPair(blue_channel);
  }

  private hexPair(value: number): string {
    const text: string = value.toString(16);
    return text.length === 1 ? '0' + text : text;
  }

  private cellText(row: number, column: number): string {
    if (row === column) {
      return 'x';
    }
    const value: number = this.cells[row * this.n + column];
    if (Number.isNaN(value)) {
      return 'N/A';
    }
    if (value <= 0) {
      return '·';
    }
    return value.toFixed(1);
  }

  private cellBackground(row: number, column: number): string {
    if (row === column) {
      return '#EEF2F6';
    }
    const value: number = this.cells[row * this.n + column];
    if (Number.isNaN(value)) {
      return '#D92D20';
    }
    if (value <= 0) {
      return '#F2F4F7';
    }
    return this.heatColor(value);
  }

  private cellForeground(row: number, column: number): string {
    if (row === column) {
      return TEXT_TERTIARY;
    }
    const value: number = this.cells[row * this.n + column];
    if (Number.isNaN(value)) {
      return '#FFFFFF';
    }
    if (value <= 0) {
      return TEXT_TERTIARY;
    }
    return '#FFFFFF';
  }

  private indices(): number[] {
    const list: number[] = [];
    for (let index = 0; index < this.n; ++index) {
      list.push(index);
    }
    return list;
  }

  private elapsedSeconds(): number {
    if (this.startMillis === 0) {
      return 0;
    }
    return (Date.now() - this.startMillis) / 1000;
  }

  private timestamp(): string {
    const now: Date = new Date();
    const pad = (value: number): string => value < 10 ? '0' + value : '' + value;
    return '' + now.getFullYear() + pad(now.getMonth() + 1) + pad(now.getDate()) +
      '_' + pad(now.getHours()) + pad(now.getMinutes()) + pad(now.getSeconds());
  }

  private describeError(code: number): string {
    if (code === 1) {
      return '已有测试在运行';
    }
    if (code === 2) {
      return '核心数无效';
    }
    if (code === 3) {
      return '迭代次数无效';
    }
    if (code === 4) {
      return '采样轮数无效';
    }
    if (code === 6) {
      return '无法创建测量线程';
    }
    return '原生错误 ' + code;
  }

  // -------------------------------------------------------------------------
  // 界面
  // -------------------------------------------------------------------------

  @Builder
  private sectionTitle(label: string) {
    Text(label)
      .fontSize(12)
      .fontWeight(FontWeight.Medium)
      .fontColor(TEXT_TERTIARY)
      .letterSpacing(1)
      .margin({ bottom: 8 })
  }

  @Builder
  private infoCard() {
    Column() {
      this.sectionTitle('设备信息')
      Text(this.logicalCores + ' 个逻辑线程 · ' + this.cacheLine + ' B 缓存行 · ' + this.abi)
        .fontSize(13)
        .fontColor(TEXT_SECONDARY)
        .fontFamily('monospace')
        .width('100%')
      if (this.physicalCount > 0) {
        Text('物理核 ' + this.physicalCount + ' 个' + (this.hasSmt ?
          ' · 同核超线程：' + this.smtText : ''))
          .fontSize(11.5)
          .fontColor(TEXT_SECONDARY)
          .margin({ top: 6 })
          .width('100%')
      }
      if (this.n < this.logicalCores) {
        Text('可稳定绑定 ' + this.n + ' 个线程，另有 ' +
          (this.logicalCores - this.n) + ' 个线程未纳入矩阵（系统限制）')
          .fontSize(11.5)
          .fontColor(TEXT_TERTIARY)
          .margin({ top: 6 })
          .width('100%')
      }
      if (this.hasSmt) {
        Text('测量对象：' + this.coreLabel() + ' 共 ' + this.n + ' 个逻辑线程；同核两个线程之间的延迟通常约 30 ns')
          .fontSize(11.5)
          .fontColor(TEXT_TERTIARY)
          .margin({ top: 6 })
          .width('100%')
      }
      Text('固定参数：' + this.n + ' 个逻辑线程 · 1,000,000 次 · 3 轮 · Bounce')
        .fontSize(11.5)
        .fontColor(TEXT_TERTIARY)
        .margin({ top: 6 })
        .width('100%')
      Text(this.versionText)
        .fontSize(11)
        .fontColor(TEXT_TERTIARY)
        .margin({ top: 4 })
        .width('100%')
    }
    .alignItems(HorizontalAlign.Start)
    .padding(16)
    .width('100%')
    .backgroundColor(CARD)
    .borderRadius(14)
    .margin({ top: 16 })
  }

  @Builder
  private matrixCard() {
    Column() {
      Row() {
        Text(this.caption)
          .fontSize(12)
          .fontWeight(FontWeight.Medium)
          .fontColor(TEXT_TERTIARY)
          .letterSpacing(1)
        Blank()
        Text(this.donePairs + '/' + Math.max(0, this.totalPairs))
          .fontSize(11.5)
          .fontColor(TEXT_TERTIARY)
      }
      .width('100%')
      .margin({ bottom: 10 })

      Scroll() {
        Column() {
          Row() {
            Text('')
              .width(this.matrixLabelWidth())
            ForEach(this.indices(), (column: number) => {
              Text(this.coreHeading(column))
                .fontSize(this.matrixHeaderFontSize())
                .fontColor(TEXT_TERTIARY)
                .width(this.matrixCellWidth())
                .textAlign(TextAlign.Center)
                .margin({ right: this.matrixCellGap() })
            }, (column: number) => 'h' + column)
          }
          .margin({ bottom: 4 })

          ForEach(this.indices(), (row: number) => {
            Row() {
              Text(this.coreHeading(row))
                .fontSize(this.matrixHeaderFontSize())
                .fontColor(TEXT_TERTIARY)
                .width(this.matrixLabelWidth())
              ForEach(this.indices(), (column: number) => {
                Text(this.cellText(row, column))
                  .fontSize(this.matrixFontSize())
                  .fontColor(this.cellForeground(row, column))
                  .backgroundColor(this.cellBackground(row, column))
                  .borderRadius(5)
                  .width(this.matrixCellWidth())
                  .height(26)
                  .textAlign(TextAlign.Center)
                  .margin({ right: this.matrixCellGap(), bottom: 3 })
              }, (column: number) => 'c' + row + '_' + column)
            }
          }, (row: number) => 'r' + row)
        }
        .alignItems(HorizontalAlign.Start)
      }
      .scrollable(ScrollDirection.Horizontal)
      .scrollBar(BarState.Off)
      .width('100%')

      Row() {
        Text('低').fontSize(10.5).fontColor(TEXT_TERTIARY)
        Row()
          .width(72)
          .height(6)
          .borderRadius(3)
          .linearGradient({
            angle: 90,
            colors: [['#12B76A', 0.0], ['#F79009', 0.5], ['#F04438', 1.0]]
          })
          .margin({ left: 6, right: 6 })
        Text('高').fontSize(10.5).fontColor(TEXT_TERTIARY)
        Blank()
        Text('平均 ' + this.average().toFixed(1) + ' · 最小 ' + this.minimum().toFixed(1) +
          ' · 最大 ' + this.maximum().toFixed(1) + ' ns')
          .fontSize(10.5)
          .fontColor(TEXT_SECONDARY)
      }
      .width('100%')
      .margin({ top: 10 })

      if (this.countNaPairs() > 0) {
        Text('N/A = 该线程对未能精确绑定到指定逻辑线程')
          .fontSize(11)
          .fontColor(TEXT_TERTIARY)
          .margin({ top: 6 })
          .width('100%')
      }
      if (this.hasSmt) {
        Text('带 * 的线程互为同核超线程（' + this.smtText + '）；这些格子通常约 30 ns')
          .fontSize(11)
          .fontColor(TEXT_TERTIARY)
          .margin({ top: 6 })
          .width('100%')
      }
    }
    .alignItems(HorizontalAlign.Start)
    .padding(this.n >= 9 ? 8 : 12)
    .width('100%')
    .backgroundColor(CARD)
    .borderRadius(14)
    .margin({ top: 16 })
  }


  build() {
    Scroll() {
      Column() {
        Text('CoherencyLatency')
          .fontSize(27)
          .fontWeight(FontWeight.Bold)
          .fontColor(TEXT_PRIMARY)
          .width('100%')
        Text('缓存一致性延迟测试 · HarmonyOS 原生版')
          .fontSize(13)
          .fontColor(TEXT_SECONDARY)
          .margin({ top: 2 })
          .width('100%')

        this.infoCard()

        Row() {
          Button(this.running ? '测试中…' : '开始测试')
            .fontSize(15)
            .fontWeight(FontWeight.Bold)
            .fontColor('#FFFFFF')
            .backgroundColor(ACCENT)
            .borderRadius(12)
            .height(48)
            .layoutWeight(2)
            .enabled(!this.running && this.n >= 2)
            .opacity(this.running || this.n < 2 ? 0.45 : 1)
            .onClick(() => {
              this.startTest();
            })

          Button('停止')
            .fontSize(15)
            .fontWeight(FontWeight.Bold)
            .fontColor(TEXT_PRIMARY)
            .backgroundColor(STOP_BACKGROUND)
            .borderRadius(12)
            .border({ width: 1, color: BORDER })
            .height(48)
            .layoutWeight(1)
            .margin({ left: 10 })
            .enabled(this.running)
            .opacity(this.running ? 1 : 0.5)
            .onClick(() => {
              this.stopTest();
            })
        }
        .width('100%')
        .margin({ top: 16 })

        Progress({ value: this.donePairs, total: Math.max(1, this.totalPairs), type: ProgressType.Linear })
          .color(ACCENT)
          .backgroundColor(BORDER)
          .height(6)
          .width('100%')
          .margin({ top: 18 })

        Text(this.statusText)
          .fontSize(12.5)
          .fontColor(TEXT_SECONDARY)
          .margin({ top: 8 })
          .width('100%')

        this.matrixCard()

        Row() {
          Button('复制 CSV')
            .fontSize(15)
            .fontWeight(FontWeight.Bold)
            .fontColor(ACCENT)
            .backgroundColor(CARD)
            .borderRadius(12)
            .border({ width: 1, color: ACCENT_BORDER })
            .height(48)
            .layoutWeight(1)
            .enabled(this.hasResult && !this.running)
            .opacity(this.hasResult && !this.running ? 1 : 0.5)
            .onClick(() => {
              this.copyCsv();
            })

          Button('另存一份')
            .fontSize(15)
            .fontWeight(FontWeight.Bold)
            .fontColor(ACCENT_DARK)
            .backgroundColor(CARD)
            .borderRadius(12)
            .border({ width: 1, color: ACCENT_BORDER })
            .height(48)
            .layoutWeight(1)
            .margin({ left: 10 })
            .enabled(this.hasResult && !this.running)
            .opacity(this.hasResult && !this.running ? 1 : 0.5)
            .onClick(() => {
              this.exportAgain();
            })
        }
        .width('100%')
        .margin({ top: 12 })

        Text(this.savedMessage)
          .fontSize(11.5)
          .fontColor(TEXT_TERTIARY)
          .margin({ top: 6 })
          .width('100%')

        Text('数值是每次原子操作的往返延迟，越低越好。导出后可导入表格软件。')
          .fontSize(11.5)
          .fontColor(TEXT_TERTIARY)
          .margin({ top: 18 })
          .width('100%')
      }
      .alignItems(HorizontalAlign.Start)
      .padding({ left: this.n >= 9 ? 10 : 16, right: this.n >= 9 ? 10 : 16, top: 18, bottom: 30 })
      .width('100%')
    }
    .scrollBar(BarState.Auto)
    .width('100%')
    .height('100%')
    .backgroundColor(BACKGROUND)
  }
}