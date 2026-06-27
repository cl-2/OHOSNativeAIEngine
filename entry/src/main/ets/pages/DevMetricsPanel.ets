/**
 * DevMetricsPanel.ets - 增强版性能监控面板
 *
 * 功能:
 * - 9 项指标 MiniCard（最新值 + 均值）
 * - 6 种图表 Tab 切换
 * - 拖拽 + 导出 + 重置
 * - min/max/avg 标签
 */

import nativeLib from 'libnative_lib.so';
import { MetricsCollector, MetricTypeId } from '../utils/MetricsCollector';
import { BenchmarkRunner } from '../utils/BenchmarkRunner';
import { fileIo } from '@kit.CoreFileKit';

const PRIMARY_COLOR: string = '#6366F1';

interface ChartTab {
  label: string;
  typeId: number;
  color: string;
  unit: string;
}

const CHART_TABS: ChartTab[] = [
  { label: 'RTF', typeId: 0, color: '#6366F1', unit: '' },
  { label: 'ASR', typeId: 1, color: '#F59E0B', unit: 'ms' },
  { label: 'VAD', typeId: 2, color: '#10B981', unit: 'ms' },
  { label: 'TTS', typeId: 4, color: '#EF4444', unit: 'ms' },
  { label: 'LLM', typeId: 8, color: '#8B5CF6', unit: 'ms' },
  { label: 'Buf', typeId: 6, color: '#06B6D4', unit: '%' },
];

@Component
export struct DevMetricsPanel {
  @Link showPanel: boolean;
  @Link isExpanded: boolean;

  @State panelX: number = 16;
  @State panelY: number = 80;
  private dragBaseX: number = 0;
  private dragBaseY: number = 0;

  @State metrics: number[] = new Array<number>(9).fill(0);
  @State summaries: Record<string, Object> = {};
  @State chartTabIdx: number = 0;
  @State chartData: number[] = [];
  @State exportLog: string = '';
  @State hasData: boolean = false;

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
    // 只检查关键指标判断是否有真实数据（RTF=0, ASR=1, VAD=2, TTS=4, LLM=8）
    const keyMetrics: number[] = [0, 1, 2, 4, 8];
    for (let i: number = 0; i < 9; i++) {
      try {
        const v: number = MetricsCollector.getLatest(i);
        this.metrics[i] = v;
      } catch (_) { this.metrics[i] = 0; }
    }
    for (let i: number = 0; i < keyMetrics.length; i++) {
      const v: number = this.metrics[keyMetrics[i]];
      if (v > 0) { anyData = true; break; }
    }
    for (let i: number = 0; i < 9; i++) {
      try {
        const s: Object = MetricsCollector.getSummary(i);
        const r = s as Record<string, Object>;
        // 只有 count>0 且 latest>0 才认为是有效数据
        if ((r['count'] as number) > 0 && (r['latest'] as number) > 0) {
          this.summaries[i.toString()] = s;
        }
      } catch (_) {}
    }
    this.hasData = anyData;
    this.refreshChart();
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
    .width(this.showPanel && this.isExpanded ? 280 : 0)
    .height(this.showPanel && this.isExpanded ? 400 : 0)
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
        Text('📊 性能仪表盘').fontSize(13).fontWeight('600').fontColor('#374151')
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
          // ===== 9 项指标卡片 =====
          Row({ space: 4 }) {
            this.MiniCard('RTF',  this.metrics[0].toFixed(3), this.getAvg(0).toFixed(3), this.getAvg(0) > 0)
            this.MiniCard('ASR',  this.metrics[1].toFixed(0) + 'ms', this.getAvg(1).toFixed(0) + 'ms', this.getAvg(1) > 0)
            this.MiniCard('VAD',  this.metrics[2].toFixed(1) + 'ms', this.getAvg(2).toFixed(1) + 'ms', this.getAvg(2) > 0)
            this.MiniCard('Vdur', this.metrics[3].toFixed(0) + 'ms', this.getAvg(3).toFixed(0) + 'ms', this.getAvg(3) > 0)
            this.MiniCard('TTS',  (this.metrics[4] / 1000).toFixed(2) + 's', (this.getAvg(4) / 1000).toFixed(2) + 's', this.getAvg(4) > 0)
          }.width('100%').margin({ top: 4 })

          Row({ space: 4 }) {
            this.MiniCard('Tlen', (this.metrics[5] / 1000).toFixed(1) + 's', (this.getAvg(5) / 1000).toFixed(1) + 's', this.getAvg(5) > 0)
            this.MiniCard('Buf',  (this.metrics[6] * 100).toFixed(0) + '%',  (this.getAvg(6) * 100).toFixed(0) + '%',  this.getAvg(6) > 0)
            this.MiniCard('Chk',  this.metrics[7].toFixed(0), this.getAvg(7).toFixed(0), this.getAvg(7) > 0)
            this.MiniCard('LLM',  this.metrics[8].toFixed(0) + 'ms', this.getAvg(8).toFixed(0) + 'ms', this.getAvg(8) > 0)
          }.width('100%').margin({ top: 4 })

          // ===== 图表 Tab 切换 =====
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

          // ===== 图表 =====
          this.MiniChart(
            CHART_TABS[this.chartTabIdx].label +
              (CHART_TABS[this.chartTabIdx].unit ? '(' + CHART_TABS[this.chartTabIdx].unit + ')' : ''),
            this.chartData, CHART_TABS[this.chartTabIdx].color
          )

          // ===== 操作按钮 =====
          Row({ space: 8 }) {
            Button() {
              Text('🔄 重置').fontSize(10).fontColor('#EF4444')
            }.height(28).layoutWeight(1).backgroundColor('#FEF2F2').borderRadius(8)
              .onClick(() => { MetricsCollector.resetAll(); this.refreshAll(); })

            Button() {
              Text('📥 导出').fontSize(10).fontColor(PRIMARY_COLOR)
            }.height(28).layoutWeight(1).backgroundColor('#EEF2FF').borderRadius(8)
              .onClick(() => { this.doExport(); })
          }.width('100%').margin({ top: 6 })

          // ===== 基准测试按钮 =====
          Row({ space: 8 }) {
            Button() {
              Text('🏁 跑基准').fontSize(10).fontColor('#10B981')
            }.height(28).layoutWeight(1).backgroundColor('#ECFDF5').borderRadius(8)
              .onClick(() => { this.runBenchmark(); })

            Button() {
              Text('📊 对比').fontSize(10).fontColor('#6366F1')
            }.height(28).layoutWeight(1).backgroundColor('#EEF2FF').borderRadius(8)
              .onClick(() => { this.showComparison(); })
          }.width('100%').margin({ top: 4 })

          if (this.exportLog) {
            Text(this.exportLog).fontSize(9).fontColor('#6B7280').margin({ top: 4 }).width('100%')
          }
        }.width('100%').padding({ left: 8, right: 8, bottom: 8 })
      }.layoutWeight(1)
    }
    .width('100%').height('100%')
  }

  private doExport(): void {
    const json: string = MetricsCollector.exportJson();
    try {
      const ctx = getContext();
      const perfDir: string = ctx.filesDir + '/perf';
      try { fileIo.mkdirSync(perfDir); } catch (_e) {}
      const now: number = new Date().getTime();
      const path: string = perfDir + '/metrics_' + now + '.json';
      const file = fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(file.fd, json);
      fileIo.closeSync(file);
      this.exportLog = '✅ 已导出: metrics_' + now + '.json';
      console.info('OHOS_Metrics: Exported to ' + path + ' (' + json.length + ' bytes)');
    } catch (e) {
      this.exportLog = '❌ 导出失败: ' + String(e);
    }
    setTimeout(() => { this.exportLog = ''; }, 3000);
  }

  // ======================== 基准测试 ========================

  private runBenchmark(): void {
    this.exportLog = '⏳ 采集中…（语音对话几次后查看结果）';
    const ts: string = new Date().toLocaleTimeString();
    const record = BenchmarkRunner.captureSnapshot('基准_' + ts);
    const m = record.metrics;
    let info: string = '🏁 基准已保存\n';
    const names: string[] = ['RTF', 'ASRms', 'VADμs', '', 'TTSms', '', '', '', 'LLMms'];
    const keys: string[] = Object.keys(m);
    for (let i: number = 0; i < keys.length; i++) {
      const v = m[keys[i]];
      if (v && v.count > 0) {
        const kid: number = parseInt(keys[i]);
        const name: string = kid >= 0 && kid < names.length ? names[kid] : keys[i];
        info += name + ': avg=' + v.avg.toFixed(2) + ' (n=' + v.count + ')\n';
      }
    }
    this.exportLog = info;
    setTimeout(() => { this.exportLog = '✅ 基准已保存，可点"对比"查看'; }, 5000);
  }

  private showComparison(): void {
    const report: string = BenchmarkRunner.compare('', '');
    AlertDialog.show({
      title: '📊 性能对比',
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
          const w: number = 264, h: number = 56, pad: number = 4;
          const pw: number = w - pad * 2, ph: number = h - pad * 2;
          const ctx: CanvasRenderingContext2D = this.chartCtx;
          ctx.clearRect(0, 0, w, h);
          if (data.length < 2) {
            ctx.font = '10px sans-serif';
            ctx.fillStyle = '#D1D5DB';
            ctx.textAlign = 'center';
            ctx.fillText('等待数据…', w / 2, h / 2 + 3);
            return;
          }
          let mn: number = data[0], mx: number = data[0];
          for (let i: number = 1; i < data.length; i++) {
            if (data[i] < mn) mn = data[i];
            if (data[i] > mx) mx = data[i];
          }
          const rng: number = Math.max(mx - mn, 0.001);
          // 参考线
          ctx.strokeStyle = '#F3F4F6'; ctx.lineWidth = 1;
          for (let y: number = 0; y < 3; y++) {
            const yy: number = pad + (ph / 3) * y;
            ctx.beginPath(); ctx.moveTo(pad, yy); ctx.lineTo(w - pad, yy); ctx.stroke();
          }
          // 折线
          ctx.strokeStyle = color; ctx.lineWidth = 1.8; ctx.beginPath();
          const sx: number = pw / Math.max(data.length - 1, 1);
          for (let i: number = 0; i < data.length; i++) {
            const x: number = pad + i * sx;
            const y: number = pad + ph - ((data[i] - mn) / rng) * ph;
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
          }
          ctx.stroke();
          // min / max 标签
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