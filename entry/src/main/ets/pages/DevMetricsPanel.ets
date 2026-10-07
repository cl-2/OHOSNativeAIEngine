/**
 * DevMetricsPanel.ets - 性能监控面板
 *
 * 功能:
 *   - 实时指标卡片（RTF/ASR/VAD/TTS/LLM）
 *   - 趋势图（6 种指标切换）
 *   - 基准测试快照（ArkTS 侧）
 *   - 原生 C++ 压测（BenchmarkEngine）
 *   - 前后对比
 */

import nativeLib from 'libnative_lib.so';
import { MetricsCollector, MetricTypeId } from '../utils/MetricsCollector';
import { BenchmarkRunner } from '../utils/BenchmarkRunner';
import { fileIo } from '@kit.CoreFileKit';
import {
  ModelFootprint,
  RuntimeMemoryManager,
  RuntimeMemorySnapshot
} from '../utils/RuntimeMemoryManager';
import { MemoryScenarioReport, MemoryScenarioRunner } from '../utils/MemoryScenarioRunner';
import { VoiceRegressionReport, VoiceRegressionRunner } from '../utils/VoiceRegressionRunner';
import {
  LocalLlmRegressionReport,
  LocalLlmRegressionRunner
} from '../utils/LocalLlmRegressionRunner';

const PRIMARY_COLOR: string = '#6366F1';

interface ChartTab {
  label: string;
  typeId: number;
  color: string;
  unit: string;
}

interface DiagnosticsExport {
  session: DiagnosticsSession;
  metrics: Object;
  memoryStages: RuntimeMemorySnapshot[];
  memoryScenarios: MemoryScenarioReport[];
  voiceRegressions: VoiceRegressionReport[];
  localLlmRegressions: LocalLlmRegressionReport[];
}

interface DiagnosticsSession {
  id: string;
  startedAt: number;
  exportedAt: number;
}

interface MetricExportFallback {
  exportParseError: string;
  rawMetrics: string;
}

const CHART_TABS: ChartTab[] = [
  { label: 'RTF', typeId: 0, color: '#6366F1', unit: '' },
  { label: 'ASR', typeId: 1, color: '#F59E0B', unit: 'ms' },
  { label: 'VAD', typeId: 2, color: '#10B981', unit: 'ms' },
  { label: 'TTS', typeId: 4, color: '#EF4444', unit: 'ms' },
  { label: 'LLM', typeId: 8, color: '#8B5CF6', unit: 'ms' },
  { label: 'Buf', typeId: 6, color: '#06B6D4', unit: '%' },
  { label: 'TTFT', typeId: 13, color: '#F97316', unit: 'ms' },
  { label: 'Tok/s', typeId: 14, color: '#14B8A6', unit: '' },
  { label: 'TTS RTF', typeId: 16, color: '#EC4899', unit: '' },
  { label: 'RSS', typeId: 18, color: '#64748B', unit: 'MB' },
];

@Component
export struct DevMetricsPanel {
  @Link showPanel: boolean;
  @Link isExpanded: boolean;

  @State panelX: number = 16;
  @State panelY: number = 80;
  private dragBaseX: number = 0;
  private dragBaseY: number = 0;

  @State metrics: number[] = new Array<number>(21).fill(0);
  @State summaries: Record<string, Object> = {};
  @State chartTabIdx: number = 0;
  @State chartData: number[] = [];
  @State exportLog: string = '';
  @State hasData: boolean = false;
  @State memoryStages: RuntimeMemorySnapshot[] = [];
  @State modelFootprintText: string = '';
  @State scenarioRunning: boolean = false;
  @State voiceRegressionRunning: boolean = false;
  @State localLlmRegressionRunning: boolean = false;

  private pollTimer: number = -1;

  aboutToAppear(): void {
    this.refreshAll();
    this.pollTimer = setInterval(() => {
      if (this.showPanel && this.isExpanded) this.refreshAll();
    }, 1000);
  }

  aboutToDisappear(): void {
    if (this.pollTimer >= 0) { clearInterval(this.pollTimer); this.pollTimer = -1; }
  }

  private refreshAll(): void {
    let anyData: boolean = false;
    const keyMetrics: number[] = [0, 1, 2, 4, 8];
    for (let i: number = 0; i < 21; i++) {
      try {
        const v: number = MetricsCollector.getLatest(i);
        this.metrics[i] = v;
      } catch (_) { this.metrics[i] = 0; }
    }
    for (let i: number = 0; i < keyMetrics.length; i++) {
      if (this.metrics[keyMetrics[i]] > 0) { anyData = true; break; }
    }
    for (let i: number = 0; i < 21; i++) {
      try {
        const s: Object = MetricsCollector.getSummary(i);
        const r = s as Record<string, Object>;
        if ((r['count'] as number) > 0 && (r['latest'] as number) > 0) {
          this.summaries[i.toString()] = s;
        }
      } catch (_) {}
    }
    this.hasData = anyData;
    this.refreshMemoryData();
    this.refreshChart();
  }

  private refreshMemoryData(): void {
    const all: RuntimeMemorySnapshot[] = RuntimeMemoryManager.getRecentSnapshots();
    this.memoryStages = all.slice(Math.max(0, all.length - 6));
    const footprint: ModelFootprint = RuntimeMemoryManager.getModelFootprint();
    this.modelFootprintText = 'ASR(INT8) ' + footprint.asrMb.toFixed(0) + 'MB · VAD ' +
      footprint.vadMb.toFixed(1) + 'MB · TTS(VITS) ' + footprint.ttsMb.toFixed(0) +
      'MB · LLM(Q4_K_M) ' + footprint.llmMb.toFixed(0) + 'MB';
  }

  private refreshChart(): void {
    const tab: ChartTab = CHART_TABS[this.chartTabIdx];
    try {
      const pts: Array<Object> = MetricsCollector.getLatestPoints(tab.typeId, 60) as Array<Object>;
      const d: number[] = [];
      for (let i: number = 0; i < pts.length; i++) {
        d[i] = (pts[i] as Record<string, Object>)['v'] as number;
      }
      this.chartData = d;
    } catch (_) { this.chartData = []; }
  }

  private getAvg(typeId: number): number {
    const s: Object = this.summaries[typeId.toString()];
    if (s) {
      const r: Record<string, Object> = s as Record<string, Object>;
      const latest: number = (r['latest'] as number) || 0;
      if (latest <= 0) return 0;
      return (r['avg'] as number) || 0;
    }
    return 0;
  }

  build(): void {
    Column() {
      if (this.showPanel && this.isExpanded) this.PanelContent()
    }
    .width(this.showPanel && this.isExpanded ? 320 : 0)
    .height(this.showPanel && this.isExpanded ? 480 : 0)
    .backgroundColor('#FFFFFF').borderRadius(16)
    .shadow({ radius: 20, offsetX: 0, offsetY: 6, color: 'rgba(0,0,0,0.18)' })
    .position({ x: this.panelX, y: this.panelY })
  }

  @Builder
  PanelContent(): void {
    Column() {
      // ===== 标题栏 =====
      Row() {
        Circle().width(8).height(8).fill(this.hasData ? '#10B981' : '#9CA3AF')
        Text(this.hasData ? '● 监控中' : '○ 无数据').fontSize(10).fontColor('#6B7280').margin({ left: 4 })
        Blank()
        Text('性能仪表盘').fontSize(13).fontWeight('600').fontColor('#374151')
        Blank()
        Text('⏺').fontSize(16).width(26).height(26).textAlign(TextAlign.Center)
          .backgroundColor('#F3F4F6').borderRadius(13)
          .onClick(() => { this.isExpanded = false; })
        Text('✕').fontSize(14).fontColor('#9CA3AF').width(26).height(26)
          .textAlign(TextAlign.Center).margin({ left: 4 })
          .backgroundColor('#F3F4F6').borderRadius(13)
          .onClick(() => { this.showPanel = false; this.isExpanded = false; })
      }
      .width('100%').height(34).alignItems(VerticalAlign.Center)
      .padding({ left: 8, right: 8 })
      .gesture(
        PanGesture()
          .onActionStart(() => { this.dragBaseX = this.panelX; this.dragBaseY = this.panelY; })
          .onActionUpdate((e: GestureEvent) => {
            this.panelX = this.dragBaseX + e.offsetX;
            this.panelY = this.dragBaseY + e.offsetY;
          })
      )

      Scroll() {
        Column() {
          this.MetricCards()
          this.MemorySection()
          this.ChartTabs()
          this.ChartSection()
          this.ActionButtons()
          this.BenchmarkSection()
          this.NativeBenchmarkSection()
          this.VoiceRegressionSection()

          if (this.exportLog) {
            Text(this.exportLog).fontSize(9).fontColor('#6B7280').margin({ top: 4 }).width('100%')
          }
        }.width('100%').padding({ left: 8, right: 8, bottom: 8 })
      }.layoutWeight(1)
    }
    .width('100%').height('100%')
  }

  @Builder
  MemorySection(): void {
    Column() {
      Row() {
        Text('分阶段内存').fontSize(10).fontColor('#6B7280').fontWeight('500')
        Blank()
        Text('手动快照').fontSize(9).fontColor(PRIMARY_COLOR)
          .onClick((): void => {
            RuntimeMemoryManager.capture('手动快照');
            this.refreshMemoryData();
          })
      }.width('100%').margin({ top: 6 })
      Text(this.modelFootprintText).fontSize(8).fontColor('#9CA3AF')
        .width('100%').margin({ top: 2, bottom: 2 })
      ForEach(this.memoryStages, (item: RuntimeMemorySnapshot): void => {
        Row() {
          Text(item.stage).fontSize(8).fontColor('#6B7280').layoutWeight(1)
            .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          Text(item.rssMb.toFixed(0) + 'MB').fontSize(8).fontColor('#1F2937').width(42)
          Text((item.asrLoaded ? 'A' : '-') + (item.ttsLoaded ? 'T' : '-') +
            (item.llmLoaded ? 'L' : '-')).fontSize(8).fontColor('#6366F1').width(28)
        }.width('100%').height(17)
      }, (item: RuntimeMemorySnapshot): string => item.timestamp.toString() + item.stage)
      if (this.memoryStages.length === 0) {
        Text('等待阶段快照…').fontSize(8).fontColor('#D1D5DB').width('100%')
      }
      Text('量化评估：结合 LLM 文件大小、TTFT 与 Tok/s 对比，不自动替换模型')
        .fontSize(8).fontColor('#F59E0B').width('100%').margin({ top: 2 })
      Button() {
        Text(this.scenarioRunning ? '内存场景压测运行中…' : '运行内存场景压测（约1–3分钟）')
          .fontSize(9).fontColor('#FFFFFF')
      }
      .width('100%').height(28).margin({ top: 5 })
      .backgroundColor(this.scenarioRunning ? '#9CA3AF' : '#6366F1').borderRadius(7)
      .enabled(!this.scenarioRunning && !this.localLlmRegressionRunning)
      .onClick((): void => this.runMemoryScenario())
    }
    .width('100%')
    .padding({ left: 5, right: 5, top: 3, bottom: 3 })
    .backgroundColor('#F8F9FC')
    .borderRadius(8)
  }

  @Builder
  MetricCards(): void {
    Row({ space: 4 }) {
      this.MiniCard('RTF',  this.metrics[0].toFixed(3), this.getAvg(0).toFixed(3), this.getAvg(0) > 0)
      this.MiniCard('ASR',  this.metrics[1].toFixed(0) + 'ms', this.getAvg(1).toFixed(0) + 'ms', this.getAvg(1) > 0)
      this.MiniCard('VAD',  this.metrics[2].toFixed(1) + 'ms', this.getAvg(2).toFixed(1) + 'ms', this.getAvg(2) > 0)
      this.MiniCard('TTS',  (this.metrics[4] / 1000).toFixed(2) + 's', (this.getAvg(4) / 1000).toFixed(2) + 's', this.getAvg(4) > 0)
    }.width('100%').margin({ top: 4 })
    Row({ space: 4 }) {
      this.MiniCard('TTFT', this.metrics[13].toFixed(0) + 'ms', this.getAvg(13).toFixed(0) + 'ms', this.getAvg(13) > 0)
      this.MiniCard('Tok/s', this.metrics[14].toFixed(1), this.getAvg(14).toFixed(1), this.getAvg(14) > 0)
      this.MiniCard('TTS RTF', this.metrics[16].toFixed(2), this.getAvg(16).toFixed(2), this.getAvg(16) > 0)
      this.MiniCard('RSS', this.metrics[18].toFixed(0) + 'MB', this.getAvg(18).toFixed(0) + 'MB', this.getAvg(18) > 0)
    }.width('100%').margin({ top: 4 })
    Row({ space: 4 }) {
      this.MiniCard('LLM',  this.metrics[8].toFixed(0) + 'ms', this.getAvg(8).toFixed(0) + 'ms', this.getAvg(8) > 0)
      this.MiniCard('Buf',  (this.metrics[6] * 100).toFixed(0) + '%',  (this.getAvg(6) * 100).toFixed(0) + '%',  this.getAvg(6) > 0)
      this.MiniCard('Pipe', this.metrics[9].toFixed(1) + 'ms', this.getAvg(9).toFixed(1) + 'ms', this.getAvg(9) > 0)
      this.MiniCard('DC',   this.metrics[10].toFixed(0) + 'us', this.getAvg(10).toFixed(0) + 'us', this.getAvg(10) > 0)
    }.width('100%').margin({ top: 4 })
  }

  @Builder
  ChartTabs(): void {
    Row({ space: 4 }) {
      ForEach(CHART_TABS, (tab: ChartTab, idx: number) => {
        Text(tab.label)
          .fontSize(10).fontWeight(this.chartTabIdx === idx ? '700' : '400')
          .fontColor(this.chartTabIdx === idx ? '#FFFFFF' : '#6B7280')
          .backgroundColor(this.chartTabIdx === idx ? tab.color : '#F3F4F6')
          .borderRadius(10).padding({ left: 8, right: 8, top: 3, bottom: 3 })
          .onClick(() => { this.chartTabIdx = idx; this.refreshChart(); })
      }, (tab: ChartTab, idx: number): string => 'ct_' + idx)
    }.width('100%').margin({ top: 6 })
  }

  @Builder
  ChartSection(): void {
    this.MiniChart(
      CHART_TABS[this.chartTabIdx].label +
        (CHART_TABS[this.chartTabIdx].unit ? '(' + CHART_TABS[this.chartTabIdx].unit + ')' : ''),
      this.chartData, CHART_TABS[this.chartTabIdx].color
    )
  }

  @Builder
  ActionButtons(): void {
    Row({ space: 8 }) {
      Button() {
        Text('重置').fontSize(10).fontColor('#EF4444')
      }.height(28).layoutWeight(1).backgroundColor('#FEF2F2').borderRadius(8)
        .onClick(() => { MetricsCollector.resetAll(); this.refreshAll(); })
      Button() {
        Text('导出').fontSize(10).fontColor(PRIMARY_COLOR)
      }.height(28).layoutWeight(1).backgroundColor('#EEF2FF').borderRadius(8)
        .onClick(() => { this.doExport(); })
      Button() {
        Text('报告').fontSize(10).fontColor('#10B981')
      }.height(28).layoutWeight(1).backgroundColor('#ECFDF5').borderRadius(8)
        .onClick(() => { this.showReport(); })
    }.width('100%').margin({ top: 6 })
    Row({ space: 8 }) {
      Text('TTS线程').fontSize(9).fontColor('#6B7280').layoutWeight(1)
      Button('2线程').height(26).fontSize(9).backgroundColor('#F3F4F6').fontColor('#374151')
        .onClick(() => { this.selectTtsThreads(2); })
      Button('4线程').height(26).fontSize(9).backgroundColor('#EEF2FF').fontColor(PRIMARY_COLOR)
        .onClick(() => { this.selectTtsThreads(4); })
    }.width('100%').margin({ top: 4 })
  }

  @Builder
  BenchmarkSection(): void {
    Text('ArkTS 快照基准').fontSize(10).fontColor('#6B7280').fontWeight('500').margin({ top: 6 })
    Row({ space: 8 }) {
      Button() {
        Text('取快照').fontSize(10).fontColor('#10B981')
      }.height(28).layoutWeight(1).backgroundColor('#ECFDF5').borderRadius(8)
        .onClick(() => { this.runBenchmark(); })
      Button() {
        Text('前后对比').fontSize(10).fontColor('#6366F1')
      }.height(28).layoutWeight(1).backgroundColor('#EEF2FF').borderRadius(8)
        .onClick(() => { this.showComparison(); })
    }.width('100%').margin({ top: 4 })
  }

  @Builder
  NativeBenchmarkSection(): void {
    Text('C++ 原生压测').fontSize(10).fontColor('#6B7280').fontWeight('500').margin({ top: 6 })
    Row({ space: 8 }) {
      Button() {
        Text('跑压测').fontSize(10).fontColor('#8B5CF6')
      }.height(28).layoutWeight(1).backgroundColor('#F5F3FF').borderRadius(8)
        .onClick(() => { this.runNativeBench(); })
      Button() {
        Text('对比上次').fontSize(10).fontColor('#10B981')
      }.height(28).layoutWeight(1).backgroundColor('#ECFDF5').borderRadius(8)
        .onClick(() => { this.showNativeComparison(); })
    }.width('100%').margin({ top: 4 })
  }

  @Builder
  VoiceRegressionSection(): void {
    Text('语音逻辑回归').fontSize(10).fontColor('#6B7280').fontWeight('500').margin({ top: 6 })
    Button() {
      Text(this.voiceRegressionRunning ? '正在运行…' : '运行确定性回归')
        .fontSize(10).fontColor('#0F766E')
    }.height(28).width('100%').backgroundColor('#CCFBF1').borderRadius(8)
      .enabled(!this.voiceRegressionRunning && !this.scenarioRunning &&
        !this.localLlmRegressionRunning)
      .onClick((): void => this.runVoiceRegression())
    Button() {
      Text(this.localLlmRegressionRunning ? '真实 GGUF 回归运行中…' : '运行真实 GGUF 回归（约10～30秒）')
        .fontSize(10).fontColor('#7C3AED')
    }.height(28).width('100%').margin({ top: 4 }).backgroundColor('#F5F3FF').borderRadius(8)
      .enabled(!this.localLlmRegressionRunning && !this.voiceRegressionRunning && !this.scenarioRunning)
      .onClick((): void => this.runLocalLlmRegression())
  }

  // ======================== 操作 ========================

  private doExport(): void {
    try {
      const ctx = getContext();
      const perfDir: string = ctx.filesDir + '/perf';
      try { fileIo.mkdirSync(perfDir); } catch (_e) {}
      const now: number = new Date().getTime();
      MemoryScenarioRunner.init(ctx.filesDir);
      VoiceRegressionRunner.init(ctx.filesDir);
      LocalLlmRegressionRunner.init(ctx.filesDir);
      const rawMetrics: string = MetricsCollector.exportJson();
      let metrics: Object;
      try {
        metrics = JSON.parse(rawMetrics) as Object;
      } catch (parseError) {
        // Never lose the scenario report just because a legacy native metric
        // export contains malformed JSON. The next native build fixes it.
        const fallback: MetricExportFallback = {
          exportParseError: String(parseError),
          rawMetrics,
        };
        metrics = fallback as Object;
      }
      const payload: DiagnosticsExport = {
        session: {
          id: MetricsCollector.getSessionId(),
          startedAt: MetricsCollector.getSessionStartedAt(),
          exportedAt: now,
        },
        metrics,
        memoryStages: RuntimeMemoryManager.getRecentSnapshots(),
        memoryScenarios: MemoryScenarioRunner.getHistory(),
        voiceRegressions: VoiceRegressionRunner.getHistory(),
        localLlmRegressions: LocalLlmRegressionRunner.getHistory(),
      };
      const path: string = perfDir + '/diagnostics_' + now + '.json';
      const file = fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(file.fd, JSON.stringify(payload));
      fileIo.closeSync(file);
      this.exportLog = '已导出 diagnostics_' + now + '.json';
    } catch (e) {
      this.exportLog = '导出失败: ' + String(e);
    }
    setTimeout(() => { this.exportLog = ''; }, 3000);
  }

  private showReport(): void {
    const report: string = nativeLib.getBenchmarkReport() as string;
    AlertDialog.show({
      title: '性能报告',
      message: report,
      autoCancel: true,
      confirm: { value: '关闭', action: (): void => {} },
    });
  }

  private runBenchmark(): void {
    const ts: string = new Date().toLocaleTimeString();
    const record = BenchmarkRunner.captureSnapshot('基准_' + ts);
    const m = record.metrics;
    let info: string = '快照已保存\n';
    const keys: string[] = Object.keys(m);
    for (let i: number = 0; i < keys.length; i++) {
      const v = m[keys[i]];
      if (v && v.count > 0) {
        info += ' [' + keys[i] + '] avg=' + v.avg.toFixed(3) + ' (n=' + v.count + ')';
      }
    }
    this.exportLog = info;
    setTimeout(() => { this.exportLog = '可点"前后对比"查看'; }, 4000);
  }

  private showComparison(): void {
    const report: string = BenchmarkRunner.compare('', '');
    AlertDialog.show({
      title: '性能对比',
      message: report,
      autoCancel: true,
      confirm: { value: '关闭', action: (): void => {} },
    });
  }

  private runNativeBench(): void {
    this.exportLog = 'C++ 压测中 (1000帧)...';
    try {
      const result = BenchmarkRunner.runNativeBenchmark('压测_' + new Date().toLocaleTimeString());
      this.exportLog = 'RSS:' + result.memory.rss_mb.toFixed(0) + 'MB '
        + 'Pipe:' + result.pipeline_benchmark.latency_ms.avg.toFixed(3) + 'ms '
        + '帧/秒:' + result.pipeline_benchmark.throughput.frames_per_sec.toFixed(0);
    } catch (e) {
      this.exportLog = '压测失败: ' + String(e);
    }
    setTimeout(() => { this.exportLog = '可点"对比上次"查看详情'; }, 5000);
  }

  private runMemoryScenario(): void {
    if (this.scenarioRunning || this.localLlmRegressionRunning) return;
    this.scenarioRunning = true;
    this.exportLog = '内存场景压测开始：请勿对话、切换模型或退出应用…';
    try {
      MemoryScenarioRunner.init(getContext().filesDir);
      MemoryScenarioRunner.run((status: string): void => {
        this.exportLog = status;
      }).then((report: MemoryScenarioReport): void => {
        this.scenarioRunning = false;
        this.refreshAll();
        if (!report.success) {
          this.exportLog = '场景压测失败：' + report.error;
          return;
        }
        const llmBefore: number = report.samples[0]?.rssMedianMb || 0;
        const llmLoaded: number = report.samples[1]?.rssMedianMb || 0;
        const llmReleased: number = report.samples[3]?.rssMedianMb || 0;
        const ttsLoaded: number = report.samples[4]?.rssMedianMb || 0;
        const ttsReleased: number = report.samples[5]?.rssMedianMb || 0;
        const llmFirstToken: number = report.localRuns[0]?.firstTokenMs || 0;
        const ttsColdFirst: number = report.ttsRuns[0]?.firstChunkMs || 0;
        const ttsWarmFirst: number = report.ttsRuns[1]?.firstChunkMs || 0;
        this.exportLog = '完成：LLM +' + (llmLoaded - llmBefore).toFixed(1) + 'MB，释放 ' +
          (llmLoaded - llmReleased).toFixed(1) + 'MB；TTS +' +
          (ttsLoaded - llmReleased).toFixed(1) + 'MB，释放 ' +
          (ttsLoaded - ttsReleased).toFixed(1) + 'MB；LLM首Token ' + llmFirstToken +
          'ms，TTS首包 冷/热 ' + ttsColdFirst + '/' + ttsWarmFirst +
          'ms。结果已保存 memory_scenarios.json';
      }).catch((e: Error): void => {
        this.scenarioRunning = false;
        this.exportLog = '场景压测异常：' + String(e);
      });
    } catch (e) {
      this.scenarioRunning = false;
      this.exportLog = '场景压测启动失败：' + String(e);
    }
  }

  private runVoiceRegression(): void {
    if (this.voiceRegressionRunning || this.localLlmRegressionRunning) return;
    this.voiceRegressionRunning = true;
    this.exportLog = '正在校验语气词、状态机、打断握手和端云路由…';
    try {
      VoiceRegressionRunner.init(getContext().filesDir);
      const report: VoiceRegressionReport = VoiceRegressionRunner.run();
      this.exportLog = (report.success ? '通过：' : '失败：') + report.passed + '通过 / ' +
        report.failed + '失败；' + (report.persisted ? '结果已写入 voice_regressions.json' :
          '磁盘保存失败，但本次结果仍可随诊断导出：' + report.persistError);
    } catch (e) {
      this.exportLog = '语音逻辑回归异常：' + String(e);
    }
    this.voiceRegressionRunning = false;
    setTimeout((): void => { this.exportLog = ''; }, 6000);
  }

  private runLocalLlmRegression(): void {
    if (this.localLlmRegressionRunning) return;
    this.localLlmRegressionRunning = true;
    this.exportLog = '真实 GGUF 回归开始：期间请勿语音对话或切换模型…';
    try {
      LocalLlmRegressionRunner.init(getContext().filesDir);
      LocalLlmRegressionRunner.run((status: string): void => {
        this.exportLog = status;
      }).then((report: LocalLlmRegressionReport): void => {
        this.localLlmRegressionRunning = false;
        this.refreshAll();
        const disk: string = report.persisted ? '结果已写入 local_llm_regressions.json' :
          '保存失败：' + report.persistError;
        this.exportLog = (report.success ? '真实GGUF通过：' : '真实GGUF失败：') +
          report.passed + '通过 / ' + report.failed + '失败，重复=' +
          report.duplicateResponses + '；' + disk;
      }).catch((e: Error): void => {
        this.localLlmRegressionRunning = false;
        this.exportLog = '真实 GGUF 回归异常：' + String(e);
      });
    } catch (e) {
      this.localLlmRegressionRunning = false;
      this.exportLog = '真实 GGUF 回归启动失败：' + String(e);
    }
  }

  private selectTtsThreads(numThreads: number): void {
    try {
      const applied: number = nativeLib.setTtsNumThreads(numThreads);
      this.exportLog = 'TTS 已切换为 ' + applied + ' 线程；请用相同文本各测试3轮，对比 TTS RTF';
    } catch (e) {
      this.exportLog = 'TTS 线程切换失败: ' + String(e);
    }
  }

  private showNativeComparison(): void {
    const report: string = BenchmarkRunner.compareLastTwo();
    AlertDialog.show({
      title: 'C++ 压测对比',
      message: report,
      autoCancel: true,
      confirm: { value: '关闭', action: (): void => {} },
    });
  }

  @Builder
  MiniCard(label: string, latest: string, avg: string, hasAvg: boolean): void {
    Column() {
      Text(latest).fontSize(12).fontWeight('700').fontColor('#1F2937')
      Text(label).fontSize(8).fontColor('#9CA3AF')
      if (hasAvg) {
        Text('avg ' + avg).fontSize(7).fontColor('#6B7280').margin({ top: 1 })
      }
    }
    .layoutWeight(1).alignItems(HorizontalAlign.Center)
    .padding({ top: 5, bottom: 5 }).backgroundColor('#F8F9FC').borderRadius(8)
  }

  private chartCtx: CanvasRenderingContext2D = new CanvasRenderingContext2D();

  @Builder
  MiniChart(title: string, data: number[], color: string): void {
    Column() {
      Row() {
        Text(title).fontSize(10).fontColor('#6B7280').fontWeight('500')
        Blank()
        Text(data.length + '点').fontSize(8).fontColor('#9CA3AF')
      }.width('100%').margin({ top: 6, bottom: 2 })

      Canvas(this.chartCtx)
        .width('100%').height(56)
        .backgroundColor('#FAFAFA').borderRadius(6)
        .onReady(() => {
          const w: number = 304, h: number = 56, pad: number = 4;
          const pw: number = w - pad * 2, ph: number = h - pad * 2;
          const ctx: CanvasRenderingContext2D = this.chartCtx;
          ctx.clearRect(0, 0, w, h);
          if (data.length < 2) {
            ctx.font = '10px sans-serif';
            ctx.fillStyle = '#D1D5DB';
            ctx.textAlign = 'center';
            ctx.fillText('等待数据...', w / 2, h / 2 + 3);
            return;
          }
          let mn: number = data[0], mx: number = data[0];
          for (let i: number = 1; i < data.length; i++) {
            if (data[i] < mn) mn = data[i];
            if (data[i] > mx) mx = data[i];
          }
          const rng: number = Math.max(mx - mn, 0.001);
          ctx.strokeStyle = '#F3F4F6'; ctx.lineWidth = 1;
          for (let y: number = 0; y < 3; y++) {
            const yy: number = pad + (ph / 3) * y;
            ctx.beginPath(); ctx.moveTo(pad, yy); ctx.lineTo(w - pad, yy); ctx.stroke();
          }
          ctx.strokeStyle = color; ctx.lineWidth = 1.8; ctx.beginPath();
          const sx: number = pw / Math.max(data.length - 1, 1);
          for (let i: number = 0; i < data.length; i++) {
            const x: number = pad + i * sx;
            const y: number = pad + ph - ((data[i] - mn) / rng) * ph;
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
          }
          ctx.stroke();
          ctx.font = '8px sans-serif'; ctx.textAlign = 'left';
          ctx.fillStyle = '#9CA3AF';
          ctx.fillText('min ' + mn.toFixed(2), pad + 2, h - 2);
          ctx.textAlign = 'right';
          ctx.fillText('max ' + mx.toFixed(2), w - pad - 2, 10);
        })
    }
    .width('100%')
  }
}
