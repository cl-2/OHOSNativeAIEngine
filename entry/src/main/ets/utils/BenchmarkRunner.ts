/**
 * BenchmarkRunner.ts - 性能基准测试工具
 *
 * 功能：
 * 1. 采集当前指标快照作为基线
 * 2. 运行标准 TTS 测试任务
 * 3. 持久化基准记录
 * 4. 前后对比（加速效果量化）
 */

import { MetricsCollector, MetricTypeId } from './MetricsCollector';
import { fileIo } from '@kit.CoreFileKit';

// ===== 数据结构 =====

interface MetricSnapshot {
  avg: number;
  min: number;
  max: number;
  p95: number;
  count: number;
}

interface BenchmarkRecord {
  id: string;
  label: string;
  timestamp: number;
  /** 各指标的快照 */
  metrics: { [typeId: string]: MetricSnapshot };
}

// 需要采集的指标列表
const BENCH_METRICS: { id: number; name: string; unit: string }[] = [
  { id: 0, name: 'ASR RTF',       unit: '' },
  { id: 1, name: 'ASR解码(ms)',    unit: 'ms' },
  { id: 2, name: 'VAD(μs)',        unit: 'μs' },
  { id: 4, name: 'TTS生成(ms)',    unit: 'ms' },
  { id: 8, name: 'LLM响应(ms)',    unit: 'ms' },
];

const STORAGE_PATH: string = '/benchmarks.json';

// ============================================================
// BenchmarkRunner
// ============================================================

export class BenchmarkRunner {
  private static _baseDir: string = '';

  static init(filesDir: string): void {
    this._baseDir = filesDir;
  }

  /**
   * 采集当前指标快照
   */
  static captureSnapshot(label: string): BenchmarkRecord {
    const record: BenchmarkRecord = {
      id: Date.now().toString(36),
      label,
      timestamp: Date.now(),
      metrics: {},
    };

    for (let i: number = 0; i < BENCH_METRICS.length; i++) {
      const m = BENCH_METRICS[i];
      try {
        const s: Object = MetricsCollector.getSummary(m.id);
        const r = s as Record<string, Object>;
        record.metrics[m.id.toString()] = {
          avg: (r['avg'] as number) || 0,
          min: (r['min'] as number) || 0,
          max: (r['max'] as number) || 0,
          p95: (r['p95'] as number) || 0,
          count: (r['count'] as number) || 0,
        };
      } catch (_) {
        record.metrics[m.id.toString()] = { avg: 0, min: 0, max: 0, p95: 0, count: 0 };
      }
    }

    // 保存到历史
    const all: BenchmarkRecord[] = this._loadAll();
    all.push(record);
    // 只保留最近 20 条
    const trimmed: BenchmarkRecord[] = all.slice(-20);
    this._saveAll(trimmed);

    return record;
  }

  /**
   * 获取历史基准记录
   */
  static getHistory(): BenchmarkRecord[] {
    return this._loadAll();
  }

  /**
   * 对比两个基准
   * @returns 对比文本
   */
  static compare(baselineLabel: string, currentLabel: string): string {
    const all: BenchmarkRecord[] = this._loadAll();
    let baseline: BenchmarkRecord | null = null;
    let current: BenchmarkRecord | null = null;

    for (let i: number = 0; i < all.length; i++) {
      if (all[i].label === baselineLabel) baseline = all[i];
      if (all[i].label === currentLabel) current = all[i];
    }

    if (!baseline && !current) {
      // 取最新两条
      if (all.length >= 2) {
        current = all[all.length - 1];
        baseline = all[all.length - 2];
      } else if (all.length === 1) {
        current = all[0];
      }
    }

    if (!baseline && !current) return '暂无足够基准数据';
    if (!current) current = baseline;
    if (!baseline) baseline = current;

    let report: string = '📊 性能对比报告\n';
    report += '基线: ' + baseline.label + ' (' + new Date(baseline.timestamp).toLocaleTimeString() + ')\n';
    report += '当前: ' + current.label + ' (' + new Date(current.timestamp).toLocaleTimeString() + ')\n\n';

    for (let i: number = 0; i < BENCH_METRICS.length; i++) {
      const m = BENCH_METRICS[i];
      const b = baseline.metrics[m.id.toString()];
      const c = current.metrics[m.id.toString()];
      if (!b || !c) continue;

      const bAvg: number = b.avg || 0;
      const cAvg: number = c.avg || 0;
      const diff: number = cAvg - bAvg;
      const pct: string = bAvg > 0 ? ((diff / bAvg) * 100).toFixed(1) : '—';
      const arrow: string = diff <= 0 ? '↓' : '↑';
      const unit: string = m.unit;

      report += m.name + ': ' + bAvg.toFixed(2) + unit + ' → ' + cAvg.toFixed(2) + unit;
      if (bAvg > 0) report += ' (' + arrow + ' ' + Math.abs(diff).toFixed(2) + unit + ', ' + pct + '%)';
      report += '\n';
    }

    return report;
  }

  /**
   * 删除所有基准记录
   */
  static reset(): void {
    this._saveAll([]);
  }

  // ======================== 内部 ========================

  private static _path(): string {
    return this._baseDir + STORAGE_PATH;
  }

  private static _loadAll(): BenchmarkRecord[] {
    try {
      const data: string = fileIo.readTextSync(this._path());
      return JSON.parse(data) as BenchmarkRecord[];
    } catch (_) {
      return [];
    }
  }

  private static _saveAll(records: BenchmarkRecord[]): void {
    try {
      const json: string = JSON.stringify(records, null, 2);
      const f = fileIo.openSync(this._path(), fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(f.fd, json);
      fileIo.closeSync(f);
    } catch (e) {
      console.error('OHOS_Bench: save failed: ' + String(e));
    }
  }
}
