import { pasteboard } from '@kit.BasicServicesKit';
import { deviceInfo as sysDeviceInfo } from '@kit.BasicServicesKit';
import { window, display, LengthMetrics, ColorMetrics } from '@kit.ArkUI';
import { displaySync } from '@kit.ArkGraphics2D';
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
  @State private darkMode: boolean = false;
  /** 关于 / 开源许可 / 隐私说明 弹层：0 关闭，1 关于，2 许可，3 隐私 */
  @State private aboutDlg: number = 0;
  @State private showFullLicense: boolean = false;
  /** 测试期间逐帧递增，强制 ArkUI 持续请求 vsync，避免窗口被系统判定为空闲。 */
  @State private framePulse: number = 0;
  /** 当前窗口宽度（vp）。单屏/双屏保持单列，三屏或平板/2in1 自动切双列。 */
  @State private viewWidth: number = 360;
  /** 电视：遥控器 D-pad 焦点导航 + 焦点描边。 */
  @State private tvMode: boolean = false;
  @State private roundScreen: boolean = true;
  @State private focusKey: string = '';

  // 缓存矩阵统计量：cells 每次更新只重算一次，避免每个单元格都全表扫描（O(n^4)→O(n^2)）
  private statMin: number = 0;
  private statMax: number = 0;
  private statAverage: number = 0;
  private statsValid: boolean = false;

  private timerId: number = -1;
  private resizeTimer: number = -1;
  /** 小屏（手表）点开始后自动滚到矩阵，省掉一次手动滑动。 */
  private scroller: Scroller = new Scroller();
  private pendingWidth: number = 0;
  private frameSync: displaySync.DisplaySync | null = null;
  private startMillis: number = 0;
  private lastCsv: string = '';
  private lastCsvName: string = '';
  private physicalKeys: number[] = [];
  private smtFlags: boolean[] = [];

  aboutToAppear(): void {
    try {
      const deviceType: string = sysDeviceInfo.deviceType;
      this.tvMode = deviceType === 'tv' || deviceType === 'car';
    } catch (error) {
      this.tvMode = false;
    }
    try {
      const info = display.getDefaultDisplaySync();
      this.roundScreen = info.screenShape === display.ScreenShape.ROUND;
      console.info('[CL] shape=' + info.screenShape + ' round=' + this.roundScreen);
    } catch (error) {
      this.roundScreen = true;
    }
    this.readDevice();
    this.versionText = benchVersion();
    this.applySystemTheme();
    // A 9x9 run takes tens of seconds and a 20x20 run takes minutes.  If the
    // display sleeps mid-run the platform parks cores and rewrites thread
    // placement, which is exactly when pins start failing and cells turn into
    // N/A.  Keeping the screen on for the lifetime of this page removes that
    // failure mode without asking the user to keep swiping.
    this.setKeepScreenOn(true);
  }

  // TV 遥控器焦点：电视没有触摸，必须靠方向键在四个按钮之间移动、按 OK 激活。
  private tvFocusOrder(): string[] {
    const list: string[] = [];
    if (!this.running && this.n >= 2) {
      list.push('start');
    }
    if (this.running) {
      list.push('stop');
    }
    if (this.hasResult && !this.running) {
      list.push('csv');
      list.push('save');
    }
    if (list.length === 0) {
      list.push('start');
    }
    return list;
  }

  private tvFocusId(key: string): string {
    if (key === 'start') {
      return 'btn_start';
    }
    if (key === 'stop') {
      return 'btn_stop';
    }
    if (key === 'csv') {
      return 'btn_csv';
    }
    return 'btn_save';
  }

  private tvApplyFocus(): void {
    if (!this.tvMode) {
      return;
    }
    try {
      this.getUIContext().getFocusController().requestFocus(this.tvFocusId(this.focusKey));
    } catch (error) {
    }
  }

  private tvMoveFocus(step: number): void {
    const order: string[] = this.tvFocusOrder();
    let index: number = order.indexOf(this.focusKey);
    if (index < 0) {
      index = 0;
    } else {
      index = (index + step + order.length) % order.length;
    }
    this.focusKey = order[index];
    this.tvApplyFocus();
  }

  private tvActivate(): void {
    const order: string[] = this.tvFocusOrder();
    if (order.indexOf(this.focusKey) < 0) {
      this.focusKey = order[0];
    }
    if (this.focusKey === 'start' && !this.running && this.n >= 2) {
      this.startTest();
    } else if (this.focusKey === 'stop' && this.running) {
      this.stopTest();
    } else if (this.focusKey === 'csv' && this.hasResult && !this.running) {
      this.copyCsv();
    } else if (this.focusKey === 'save' && this.hasResult && !this.running) {
      this.exportAgain();
    }
  }

  private onTvKey(code: number): void {
    if (code === 2013 || code === 2015) {
      this.tvMoveFocus(1);
    } else if (code === 2012 || code === 2014) {
      this.tvMoveFocus(-1);
    } else if (code === 2054 || code === 2055 || code === 2016 || code === 23) {
      this.tvActivate();
    }
  }

  private setKeepScreenOn(keep: boolean): void {
    try {
      window.getLastWindow(getContext(this)).then((win: window.Window) => {
        win.setWindowKeepScreenOn(keep).catch(() => {
        });
      }).catch(() => {
      });
    } catch (error) {
    }
  }

  onPageShow(): void {
    this.applySystemTheme();
    if (this.tvMode) {
      this.focusKey = 'start';
      setTimeout(() => {
        this.tvApplyFocus();
      }, 500);
    }
  }

  private applySystemTheme(): void {
    try {
      const context = getContext(this) as common.UIAbilityContext;
      // resourceManager.ColorMode.COLOR_MODE_DARK === 0
      const mode: number = context.resourceManager.getConfigurationSync().colorMode as number;
      this.darkMode = mode === 0;
    } catch (error) {
      this.darkMode = false;
    }
  }

  aboutToDisappear(): void {
    if (this.resizeTimer !== -1) {
      clearTimeout(this.resizeTimer);
      this.resizeTimer = -1;
    }
    this.setKeepScreenOn(false);
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
  // 关于 / 开源许可 / 隐私说明
  // -------------------------------------------------------------------------

  private aboutTitle(): string {
    if (this.aboutDlg === 2) {
      return this.getString('about_license_title');
    }
    if (this.aboutDlg === 3) {
      return this.getString('about_privacy_title');
    }
    return this.getString('about_about_title');
  }

  private aboutBody(): string {
    if (this.aboutDlg === 2) {
      return this.getString('about_license_body');
    }
    if (this.aboutDlg === 3) {
      return this.getString('about_privacy_body');
    }
    return this.getString('about_about_body');
  }

  private getString(name: string): string {
    try {
      return getContext(this).resourceManager.getStringByNameSync(name);
    } catch (error) {
      return '';
    }
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
      if (this.isCompact()) {
        // 手表一行只有约 220 vp，长句会被折成两行还断开词，改用短句。
        this.statusText = '就绪 · ' + this.n + ' 线程 · ' + this.n + '×' + this.n + ' 矩阵';
      } else {
        this.statusText = '就绪 · ' + this.n + ' 个逻辑线程（' + this.coreLabel() +
          '）· ' + this.n + '×' + this.n + ' 线程矩阵';
      }
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

  /** 宽屏断点：>= 840 vp（三屏态 / 平板 / 2in1）切换为双列布局。 */
  private isWide(): boolean {
    return this.viewWidth >= 840;
  }

  /** 小屏（圆形手表 466 vp 及其它 <600 vp 设备）：压缩标题与卡片内边距。 */
  private isCompact(): boolean {
    return this.viewWidth < 600;
  }

  /** 圆形表盘需要更大的左右留白，方形表盘可以贴着边框排布。 */
  private isRoundWatch(): boolean {
    return this.isCompact() && this.roundScreen;
  }

  private watchPadding(): number {
    if (!this.isCompact()) {
      return 16;
    }
    return this.roundScreen ? 30 : 12;
  }

  /** 方形表盘屏幕更矮（480x408），按钮随之收窄，避免首屏只剩按钮。 */
  private btnHeight(): number {
    if (!this.isCompact()) {
      return 48;
    }
    return this.roundScreen ? 44 : 40;
  }

  /** 方形表盘可用宽度更大，矩阵可以按实际宽度铺开。 */
  private compactMatrixWidth(): number {
    return this.roundScreen ? 150 : 196;
  }

  private matrixLabelWidth(): number {
    if (this.isCompact()) {
      return this.roundScreen ? 26 : 24;
    }
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
    const wide: boolean = this.isWide();
    if (this.isCompact()) {
      const compactAvailable: number = this.compactMatrixWidth();
      const compactComputed: number = Math.floor(
        (compactAvailable - this.matrixLabelWidth() - (this.n - 1) * this.matrixCellGap()) / Math.max(1, this.n));
      return this.roundScreen ? Math.max(20, Math.min(30, compactComputed)) : Math.max(24, Math.min(38, compactComputed));
    }
    const available: number = wide ? 640 : 332;
    const label: number = this.matrixLabelWidth();
    const gap: number = this.matrixCellGap();
    const computed: number = Math.floor(
      (available - label - (this.n - 1) * gap) / Math.max(1, this.n));
    if (this.n >= 9) {
      // The matrix card scrolls horizontally, so keep cells readable instead
      // of squeezing a 20-thread layout into the card width.
      return wide ? Math.max(28, Math.min(36, computed)) : Math.max(20, Math.min(32, computed));
    }
    if (!wide) {
      if (this.viewWidth >= 600) {
        const bigAvailable: number = Math.min(this.viewWidth, 620) - 60;
        const bigComputed: number = Math.floor(
          (bigAvailable - label - (this.n - 1) * gap) / Math.max(1, this.n));
        return Math.max(44, Math.min(72, bigComputed));
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
    // 宽屏时把 4~8 线程矩阵放大到卡片可用宽度，避免中间只占一小块。
    if (this.n === 8) {
      return Math.min(78, Math.max(56, computed));
    }
    if (this.n === 7) {
      return Math.min(90, Math.max(60, computed));
    }
    if (this.n >= 6) {
      return Math.min(100, Math.max(64, computed));
    }
    return Math.min(120, Math.max(60, computed));
  }

  private matrixCellHeight(): number {
    if (this.isCompact()) {
      return this.roundScreen ? 22 : 26;
    }
    if (this.isWide() && this.n <= 6) {
      return 56;
    }
    if (this.viewWidth >= 600 && this.n <= 6) {
      return 40;
    }
    return 26;
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
    this.statsValid = false;
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
    this.startFramePump();
    if (this.isCompact()) {
      // 手表一屏放不下：点开始后自动把矩阵滚进视野。方形表盘屏幕更矮、
      // 首屏更紧凑，滚动距离要小一些，否则矩阵的表头行会被顶出屏幕。
      const matrixOffset: number = this.roundScreen ? 210 : 170;
      setTimeout(() => {
        try {
          this.scroller.scrollTo({ xOffset: 0, yOffset: matrixOffset, animation: { duration: 300 } });
        } catch (error) {
        }
      }, 80);
    }
  }

  private startFramePump(): void {
    this.stopFramePump();
    try {
      const sync = displaySync.create();
      sync.setExpectedFrameRateRange({ min: 60, max: 120, expected: 120 });
      sync.on('frame', () => {
        this.framePulse = (this.framePulse + 1) % 1000000;
      });
      sync.start();
      this.frameSync = sync;
    } catch (error) {
      this.frameSync = null;
    }
  }

  private stopFramePump(): void {
    if (this.frameSync !== null) {
      try {
        this.frameSync.stop();
        this.frameSync.off('frame');
      } catch (error) {
      }
      this.frameSync = null;
    }
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
    this.stopFramePump();
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
        this.statsValid = false;
      }
      this.donePairs = result.done;
      this.totalPairs = result.total;
      if (result.row >= 0) {
        const rowLabel: string = 'T' + this.coreIds[result.row];
        const columnLabel: string = 'T' + this.coreIds[result.column];
        if (Number.isNaN(result.latency)) {
          this.statusText = '测量中 ' + result.done + '/' + result.total + ' · ' +
            rowLabel + '-' + columnLabel + ' N/A（绑核失败）';
        } else {
          this.statusText = '测量中 ' + result.done + '/' + result.total + ' · ' +
            rowLabel + '-' + columnLabel + ' ' + result.latency.toFixed(1) +
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
    if (this.tvMode) {
      this.focusKey = 'start';
    }
    this.stopTimer();
    this.running = false;
    const finalMatrix: number[] = benchMatrix();
    if (finalMatrix.length === this.n * this.n) {
      this.cells = finalMatrix;
      this.statsValid = false;
    }
    this.donePairs = this.totalPairs;
    this.caption = '最终结果 · 双向独立 · ns/op';
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
    if (this.tvMode) {
      this.focusKey = 'start';
    }
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

  private refreshStats(): void {
    let min: number = Number.POSITIVE_INFINITY;
    let max: number = Number.NEGATIVE_INFINITY;
    let sum: number = 0;
    let count: number = 0;
    for (let row = 0; row < this.n; ++row) {
      for (let column = 0; column < this.n; ++column) {
        if (row === column) {
          continue;
        }
        const value: number = this.cells[row * this.n + column];
        if (!Number.isNaN(value) && value > 0) {
          if (value < min) {
            min = value;
          }
          if (value > max) {
            max = value;
          }
          sum += value;
          count += 1;
        }
      }
    }
    if (count === 0) {
      this.statMin = 0;
      this.statMax = 0;
      this.statAverage = 0;
    } else {
      this.statMin = min;
      this.statMax = max;
      this.statAverage = sum / count;
    }
  }

  private minimum(): number {
    this.refreshStats();
    return this.statMin;
  }

  private maximum(): number {
    this.refreshStats();
    return this.statMax;
  }

  private average(): number {
    this.refreshStats();
    return this.statAverage;
  }

  private heatColor(value: number, min: number, max: number): string {
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

  // -------------------------------------------------------------------------
  // 主题：跟随系统深色 / 浅色
  // -------------------------------------------------------------------------

  private bgColor(): string {
    return this.darkMode ? '#121417' : '#F4F6F9';
  }

  private cardColor(): string {
    return this.darkMode ? '#1C1F24' : '#FFFFFF';
  }

  private textPrimary(): string {
    return this.darkMode ? '#E8EAED' : '#101828';
  }

  private textSecondary(): string {
    return this.darkMode ? '#A5ACB8' : '#667085';
  }

  private textTertiary(): string {
    return this.darkMode ? '#7C8492' : '#98A2B3';
  }

  private accentColor(): string {
    return this.darkMode ? '#3B82F6' : '#2563EB';
  }

  private accentDark(): string {
    return this.darkMode ? '#93B4FD' : '#1E40AF';
  }

  private accentBorder(): string {
    return this.darkMode ? '#2F4A7D' : '#BFD3FE';
  }

  private lineColor(): string {
    return this.darkMode ? '#333941' : '#E4E7EC';
  }

  private stopColor(): string {
    return this.darkMode ? '#262A30' : '#F2F4F7';
  }

  private cellEmptyColor(): string {
    return this.darkMode ? '#262A30' : '#F2F4F7';
  }

  private cellDiagonalColor(): string {
    return this.darkMode ? '#23272D' : '#EEF2F6';
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
      return this.cellDiagonalColor();
    }
    const value: number = this.cells[row * this.n + column];
    if (Number.isNaN(value)) {
      return '#D92D20';
    }
    if (value <= 0) {
      return this.cellEmptyColor();
    }
    return this.heatColor(value, this.minimum(), this.maximum());
  }

  private cellForeground(row: number, column: number): string {
    if (row === column) {
      return this.textTertiary();
    }
    const value: number = this.cells[row * this.n + column];
    if (Number.isNaN(value)) {
      return '#FFFFFF';
    }
    if (value <= 0) {
      return this.textTertiary();
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
      .fontColor(this.textTertiary())
      .letterSpacing(1)
      .margin({ bottom: 8 })
  }

  @Builder
  private infoCard() {
    Column() {
      this.sectionTitle('设备信息')
      Text(this.logicalCores + ' 个逻辑线程 · ' + this.cacheLine + ' B 缓存行 · ' + this.abi)
        .fontSize(this.isCompact() ? 11.5 : 13)
        .fontColor(this.textSecondary())
        .fontFamily('monospace')
        .textAlign(TextAlign.Start)
        .width('100%')
      if (this.physicalCount > 0) {
        Text('物理核 ' + this.physicalCount + ' 个' + (this.hasSmt ?
          ' · 同核超线程：' + this.smtText : ''))
          .fontSize(this.isCompact() ? 10 : 11.5)
          .fontColor(this.textSecondary())
          .textAlign(TextAlign.Start)
          .margin({ top: 6 })
          .width('100%')
      }
      if (this.n < this.logicalCores) {
        Text('可稳定绑定 ' + this.n + ' 个线程，另有 ' +
          (this.logicalCores - this.n) + ' 个线程未纳入矩阵（系统限制）')
          .fontSize(this.isCompact() ? 10 : 11.5)
          .fontColor(this.textTertiary())
          .textAlign(TextAlign.Start)
          .margin({ top: 6 })
          .width('100%')
      }
      if (this.hasSmt) {
        Text('测量对象：' + this.coreLabel() + ' 共 ' + this.n + ' 个逻辑线程；同核两个线程之间的延迟通常约 30 ns')
          .fontSize(this.isCompact() ? 10 : 11.5)
          .fontColor(this.textTertiary())
          .textAlign(TextAlign.Start)
          .margin({ top: 6 })
          .width('100%')
      }
      Text('固定参数：' + this.n + ' 个逻辑线程 · 1,000,000 次 · 3 轮 · Bounce')
        .fontSize(this.isCompact() ? 10 : 11.5)
        .fontColor(this.textTertiary())
        .textAlign(TextAlign.Start)
        .margin({ top: 6 })
        .width('100%')
      Text(this.versionText)
        .fontSize(this.isCompact() ? 9.5 : 11)
        .fontColor(this.textTertiary())
        .textAlign(TextAlign.Start)
        .margin({ top: 4 })
        .width('100%')
    }
    .alignItems(HorizontalAlign.Start)
    .padding(this.isCompact() ? (this.roundScreen ? 10 : 12) : 16)
    .width('100%')
    .backgroundColor(this.cardColor())
    .borderRadius(this.isCompact() ? 10 : 14)
    .margin({ top: this.isCompact() ? 10 : 16 })
  }

  @Builder
  private matrixCard() {
    Column() {
      Row() {
        Text(this.isCompact() ? '最终结果 · ns/op' : this.caption)
          .fontSize(this.isCompact() ? 10.5 : 12)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.textTertiary())
          .letterSpacing(this.isCompact() ? 0 : 1)
          .layoutWeight(1)
        Blank()
        Text(this.donePairs + '/' + Math.max(0, this.totalPairs))
          .fontSize(11.5)
          .fontColor(this.textTertiary())
      }
      .width('100%')
      .margin({ bottom: 10 })

      Scroll() {
        Row() {
        Column() {
          Row() {
            Text('')
              .width(this.matrixLabelWidth())
            ForEach(this.indices(), (column: number) => {
              Text(this.coreHeading(column))
                .fontSize(this.matrixHeaderFontSize())
                .fontColor(this.textTertiary())
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
                .fontColor(this.textTertiary())
                .width(this.matrixLabelWidth())
              ForEach(this.indices(), (column: number) => {
                Text(this.cellText(row, column))
                  .fontSize(this.matrixFontSize())
                  .fontColor(this.cellForeground(row, column))
                  .backgroundColor(this.cellBackground(row, column))
                  .borderRadius(5)
                  .width(this.matrixCellWidth())
                  .height(this.matrixCellHeight())
                  .textAlign(TextAlign.Center)
                  .margin({ right: this.matrixCellGap(), bottom: 3 })
              }, (column: number) => 'c' + row + '_' + column)
            }
          }, (row: number) => 'r' + row)
        }
        .alignItems(HorizontalAlign.Start)
        }
        .width('100%')
        .justifyContent((this.isWide() || this.viewWidth >= 600) ? FlexAlign.Center : FlexAlign.Start)
      }
      .scrollable(ScrollDirection.Horizontal)
      .scrollBar(BarState.Off)
      .width('100%')

      Row() {
        if (!this.isCompact()) {
          Text('低').fontSize(10.5).fontColor(this.textTertiary())
          Row()
            .width(72)
            .height(6)
            .borderRadius(3)
            .linearGradient({
              angle: 90,
              colors: [['#12B76A', 0.0], ['#F79009', 0.5], ['#F04438', 1.0]]
            })
            .margin({ left: 6, right: 6 })
          Text('高').fontSize(10.5).fontColor(this.textTertiary())
          Blank()
        }
        Text(this.isCompact() ?
          ('平均 ' + this.average().toFixed(1) + ' · 最大 ' + this.maximum().toFixed(1) + ' ns') :
          ('平均 ' + this.average().toFixed(1) + ' · 最小 ' + this.minimum().toFixed(1) +
            ' · 最大 ' + this.maximum().toFixed(1) + ' ns'))
          .fontSize(this.isCompact() ? 9.5 : 10.5)
          .fontColor(this.textSecondary())
      }
      .width('100%')
      .margin({ top: 10 })

      if (this.countNaPairs() > 0) {
        Text('N/A = 该线程对未能精确绑定到指定逻辑线程')
          .fontSize(11)
          .fontColor(this.textTertiary())
          .margin({ top: 6 })
          .width('100%')
      }
      if (this.hasSmt) {
        Text('带 * 的线程互为同核超线程（' + this.smtText + '）；这些格子通常约 30 ns')
          .fontSize(11)
          .fontColor(this.textTertiary())
          .margin({ top: 6 })
          .width('100%')
      }
    }
    .alignItems(HorizontalAlign.Start)
    .padding(this.isCompact() ? (this.roundScreen ? 8 : 10) : (this.n >= 9 ? 8 : 12))
    .width('100%')
    .backgroundColor(this.cardColor())
    .borderRadius(this.isCompact() ? 10 : 14)
    .margin({ top: this.isCompact() ? 10 : 16 })
  }


  // -------------------------------------------------------------------------
  // 响应式页面骨架：单屏/双屏单列，三屏/平板/2in1 双列
  // -------------------------------------------------------------------------

  @Builder
  private pageHeader() {
    Text('CoherencyLatency')
      .fontSize(this.isCompact() ? (this.roundScreen ? 19 : 17) : 27)
      .fontWeight(FontWeight.Bold)
      .fontColor(this.textPrimary())
      .textAlign(this.isCompact() ? TextAlign.Center : TextAlign.Start)
      .width('100%')
    Text(this.isCompact() && this.roundScreen ? '缓存一致性延迟测试' : '缓存一致性延迟测试 · HarmonyOS 原生版')
      .fontSize(this.isCompact() ? (this.roundScreen ? 10 : 9.5) : 13)
      .fontColor(this.textSecondary())
      .textAlign(this.isCompact() ? TextAlign.Center : TextAlign.Start)
      .margin({ top: 2 })
      .width('100%')
  }

  @Builder
  private actionSection() {
    Row() {
      Button(this.running ? '测试中…' : '开始测试')
        .fontSize(this.isCompact() ? 14 : 15)
        .fontWeight(FontWeight.Bold)
        .fontColor('#FFFFFF')
        .backgroundColor(this.accentColor())
        .borderRadius(12)
        .height(this.btnHeight())
        .layoutWeight(this.isCompact() ? 3 : 2)
        .id('btn_start')
        .border({ width: this.tvMode && this.focusKey === 'start' ? 3 : 0, color: '#FFFFFF' })
        .focusable(this.tvMode)
        .defaultFocus(this.tvMode)
        .focusBox({ strokeWidth: LengthMetrics.vp(3), strokeColor: ColorMetrics.numeric(0xFFFFFFFF), margin: LengthMetrics.vp(4) })
        .onFocus(() => { this.focusKey = 'start'; })
        .enabled(!this.running && this.n >= 2)
        .opacity(this.running || this.n < 2 ? 0.45 : 1)
        .onClick(() => {
          this.startTest();
        })

      Button('停止')
        .fontSize(this.isCompact() ? 13 : 15)
        .padding(this.isCompact() ? { left: 2, right: 2 } : { left: 16, right: 16 })
        .fontWeight(FontWeight.Bold)
        .fontColor(this.textPrimary())
        .backgroundColor(this.stopColor())
        .borderRadius(12)
        .border({ width: this.tvMode && this.focusKey === 'stop' ? 3 : 1, color: this.tvMode && this.focusKey === 'stop' ? '#FFFFFF' : this.lineColor() })
        .height(this.btnHeight())
        .layoutWeight(this.isCompact() ? 2 : 1)
        .margin({ left: 10 })
        .id('btn_stop')
        .focusable(this.tvMode)
        .focusBox({ strokeWidth: LengthMetrics.vp(3), strokeColor: ColorMetrics.numeric(0xFFFFFFFF), margin: LengthMetrics.vp(4) })
        .onFocus(() => { this.focusKey = 'stop'; })
        .enabled(this.running)
        .opacity(this.running ? 1 : 0.5)
        .onClick(() => {
          this.stopTest();
        })
    }
    .width('100%')
    .margin({ top: this.isCompact() ? 10 : 16 })

    Stack({ alignContent: Alignment.Start }) {
      Progress({ value: this.donePairs, total: Math.max(1, this.totalPairs), type: ProgressType.Linear })
        .color(this.accentColor())
        .backgroundColor(this.lineColor())
        .height(6)
        .width('100%')
      if (this.running) {
        // 测试进行中保持界面持续出帧。部分设备的电源/调度策略会把长时间
        // 静止的窗口当成空闲，并在此时收紧绑核窗口；displaySync 以 120 fps
        // 主动请求 vsync，同时每次回调都改变一个真实绘制的极小文本，确保
        // 系统看到的是持续动画而不是静止窗口，用户不必靠手指滑动来维持。
        Text(this.framePulse.toString())
          .fontSize(1)
          .width(1)
          .height(1)
          .opacity(0.02)
          .position({ x: 0, y: 0 })
      }
    }
    .height(6)
    .width('100%')
    .margin({ top: 18 })

    Text(this.statusText)
      .fontSize(this.isCompact() ? 11 : 12.5)
      .fontColor(this.textSecondary())
      .textAlign(TextAlign.Start)
      .margin({ top: this.isCompact() ? 6 : 8 })
      .width('100%')

    if (this.tvMode) {
      Text('遥控器：方向键移动焦点 · OK 键确认')
        .fontSize(11.5)
        .fontColor(this.textTertiary())
        .margin({ top: 4 })
        .width('100%')
    }
  }

  @Builder
  private exportSection() {
    Row() {
      Button('复制 CSV')
        .fontSize(15)
        .fontWeight(FontWeight.Bold)
        .fontColor(this.accentColor())
        .backgroundColor(this.cardColor())
        .borderRadius(12)
        .border({ width: this.tvMode && this.focusKey === 'csv' ? 3 : 1, color: this.tvMode && this.focusKey === 'csv' ? '#FFFFFF' : this.accentBorder() })
        .height(this.btnHeight())
        .layoutWeight(1)
        .id('btn_csv')
        .focusable(this.tvMode)
        .focusBox({ strokeWidth: LengthMetrics.vp(3), strokeColor: ColorMetrics.numeric(0xFFFFFFFF), margin: LengthMetrics.vp(4) })
        .onFocus(() => { this.focusKey = 'csv'; })
        .enabled(this.hasResult && !this.running)
        .opacity(this.hasResult && !this.running ? 1 : 0.5)
        .onClick(() => {
          this.copyCsv();
        })

      Button('另存一份')
        .fontSize(15)
        .fontWeight(FontWeight.Bold)
        .fontColor(this.accentDark())
        .backgroundColor(this.cardColor())
        .borderRadius(12)
        .border({ width: this.tvMode && this.focusKey === 'save' ? 3 : 1, color: this.tvMode && this.focusKey === 'save' ? '#FFFFFF' : this.accentBorder() })
        .height(this.btnHeight())
        .layoutWeight(1)
        .margin({ left: 10 })
        .id('btn_save')
        .focusable(this.tvMode)
        .focusBox({ strokeWidth: LengthMetrics.vp(3), strokeColor: ColorMetrics.numeric(0xFFFFFFFF), margin: LengthMetrics.vp(4) })
        .onFocus(() => { this.focusKey = 'save'; })
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
      .fontColor(this.textTertiary())
      .margin({ top: 6 })
      .width('100%')

    Text('数值是每次原子操作的往返延迟，越低越好。导出后可导入表格软件。')
      .fontSize(11.5)
      .fontColor(this.textTertiary())
      .margin({ top: 18 })
      .width('100%')
  }

  @Builder
  private footerSection() {
    Row() {
      Text('关于')
        .fontSize(12)
        .fontColor(this.textSecondary())
        .focusable(this.tvMode)
        .focusBox({ strokeWidth: LengthMetrics.vp(2), strokeColor: ColorMetrics.numeric(0xFFFFFFFF), margin: LengthMetrics.vp(2) })
        .onClick(() => {
          this.aboutDlg = 1;
        })
      Text('·').fontSize(12).fontColor(this.textTertiary()).margin({ left: 8, right: 8 })
      Text('开源许可')
        .fontSize(12)
        .fontColor(this.textSecondary())
        .onClick(() => {
          this.showFullLicense = false;
          this.aboutDlg = 2;
        })
      Text('·').fontSize(12).fontColor(this.textTertiary()).margin({ left: 8, right: 8 })
      Text('隐私说明')
        .fontSize(12)
        .fontColor(this.textSecondary())
        .onClick(() => {
          this.aboutDlg = 3;
        })
    }
    .width('100%')
    .margin({ top: 14 })

    if (this.aboutDlg > 0) {
      Column() {
        Text(this.aboutTitle())
          .fontSize(16)
          .fontWeight(FontWeight.Bold)
          .fontColor(this.textPrimary())
          .width('100%')
        Scroll() {
          Column() {
            Text(this.aboutDlg === 2 && !this.showFullLicense ?
              this.aboutBody().split('\n\n').slice(0, 1).join('\n\n') + '\n\n' +
              'Copyright (c) 2021 CapFrameX\n\n' +
              '本应用基于 MicroBenchX（MIT 许可）修改，许可全文见下。' : this.aboutBody())
              .fontSize(12.5)
              .fontColor(this.textPrimary())
              .lineHeight(20)
              .copyOption(CopyOptions.LocalDevice)
              .width('100%')
            if (this.aboutDlg === 2 && !this.showFullLicense) {
              Button('展开许可全文')
                .fontSize(13)
                .height(36)
                .margin({ top: 12 })
                .onClick(() => {
                  this.showFullLicense = true;
                })
            }
          }
          .alignItems(HorizontalAlign.Start)
          .width('100%')
        }
        .layoutWeight(1)
        .width('100%')
        .margin({ top: 12 })
        Button('关闭')
          .fontSize(14)
          .width('100%')
          .height(42)
          .margin({ top: 12 })
          .onClick(() => {
            this.aboutDlg = 0;
            this.showFullLicense = false;
          })
      }
      .alignItems(HorizontalAlign.Start)
      .padding(20)
      .width('100%')
      .height(380)
      .backgroundColor(this.cardColor())
      .borderRadius(14)
      .border({ width: 1, color: this.darkMode ? '#2A2E35' : '#E4E7EC' })
      .margin({ top: 14 })
    }
  }

  @Builder
  private narrowBody() {
    Column() {
      if (this.isCompact()) {
        // 圆形表盘：先给主操作，再给矩阵，设备信息后置，避免一屏只见说明。
        this.actionSection()
        this.matrixCard()
        this.infoCard()
      } else {
        this.infoCard()
        this.actionSection()
        this.matrixCard()
      }
      this.exportSection()
      this.footerSection()
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
  }

  @Builder
  private wideBody() {
    Row() {
      Column() {
        this.infoCard()
        this.actionSection()
        this.exportSection()
        this.footerSection()
      }
      .layoutWeight(1)
      .constraintSize({ maxWidth: 440 })
      .alignItems(HorizontalAlign.Start)

      Column() {
        this.matrixCard()
      }
      .layoutWeight(2)
      .margin({ left: 24 })
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
    .alignItems(VerticalAlign.Top)
  }

  build() {
    Row() {
      Scroll(this.scroller) {
        Column() {
          this.pageHeader()
          if (this.isWide()) {
            this.wideBody()
          } else {
            this.narrowBody()
          }
        }
        .alignItems(HorizontalAlign.Start)
        .width('100%')
        .constraintSize({ minHeight: '100%' })
        .padding({
          left: this.isCompact() ? this.watchPadding() : (this.isWide() ? 24 : (this.n >= 9 ? 10 : 16)),
          right: this.isCompact() ? this.watchPadding() : (this.isWide() ? 24 : (this.n >= 9 ? 10 : 16)),
          top: this.isCompact() ? (this.roundScreen ? 26 : 14) : 18,
          bottom: this.isCompact() ? (this.roundScreen ? 30 : 18) : 30
        })
      }
      .width('100%')
      .height('100%')
      .constraintSize({ maxWidth: this.isWide() ? 1180 : 620 })
      .scrollBar(BarState.Auto)
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .backgroundColor(this.bgColor())
    .onKeyEvent((event: KeyEvent) => {
      if (this.tvMode && event.type === KeyType.Down) {
        this.onTvKey(event.keyCode);
      }
    })
    .onAreaChange((oldValue: Area, newValue: Area) => {
      const width: number = Number(newValue.width);
      if (!Number.isNaN(width) && width > 0) {
        this.pendingWidth = width;
        if (this.resizeTimer !== -1) {
          clearTimeout(this.resizeTimer);
        }
        this.resizeTimer = setTimeout(() => {
          this.viewWidth = this.pendingWidth;
          this.resizeTimer = -1;
        }, 150);
      }
    })
  }
}
