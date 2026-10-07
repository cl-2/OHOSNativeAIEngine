/**
 * BenchmarkRunner.ts - 性能基准测试工具
 *
 * 支持两种模式：
 *   captureSnapshot() — 采集当前 MetricsCollector 历史数据的快照
 *   runNativeBenchmark() — 调用 C++ BenchmarkEngine 跑主动压测
 *
 * 使用：
 *   // 优化前跑一次
 *   const before = BenchmarkRunner.runNativeBenchmark('优化前');
 *   // 做完优化后跑一次
 *   const after = BenchmarkRunner.runNativeBenchmark('优化后');
 *   // 自动对比
 *   const report = BenchmarkRunner.compareLastTwo();
 */

import { MetricsCollector, MetricTypeId } from './MetricsCollector';
import { fileIo } from '@kit.CoreFileKit';
import nativeLib from 'libnative_lib.so';

// ===== 数据结构 =====

interface MetricSnapshot {
  avg: number;
  min: number;
  max: number;
  p95: number;
  count: number;
}

interface NativeBenchmarkResult {
  memory: {
    rss_kb: number;
    peak_kb: number;
    threads: number;
    rss_mb: number;
    peak_mb: number;
  };
  pipeline_benchmark: {
    iterations: number;
    frame_size: number;
    latency_ms: { avg: number; min: number; max: number; p50: number; p95: number; p99: number };
    throughput: { frames_per_sec: number; ms_per_frame: number };
  };
  summary: { status: string; rss_mb: number };
}

interface BenchmarkRecord {
  id: string;
  label: string;
  timestamp: number;
  type: 'snapshot' | 'native';
  metrics: { [typeId: string]: MetricSnapshot };
  nativeResult?: NativeBenchmarkResult;
}

const STORAGE_PATH: string = '/benchmarks.json';
const BENCH_METRICS: { id: number; name: string; unit: string }[] = [
  { id: 0, name: 'ASR RTF',       unit: '' },
  { id: 1, name: 'ASR解码(ms)',    unit: 'ms' },
  { id: 2, name: 'VAD(ms)',        unit: 'ms' },
  { id: 4, name: 'TTS生成(ms)',    unit: 'ms' },
  { id: 8, name: 'LLM响应(ms)',    unit: 'ms' },
  { id: 13, name: 'LLM首Token(ms)', unit: 'ms' },
  { id: 14, name: 'LLM tokens/s',  unit: 'tok/s' },
  { id: 15, name: 'TTS首包(ms)',    unit: 'ms' },
  { id: 16, name: 'TTS RTF',       unit: '' },
  { id: 17, name: 'TTS队列(ms)',    unit: 'ms' },
  { id: 18, name: 'RSS(MB)',       unit: 'MB' },
  { id: 19, name: '峰值RSS(MB)',    unit: 'MB' },
  { id: 20, name: 'TTS线程',        unit: '' },
  { id: 21, name: 'LLM prompt tokens', unit: 'token' },
  { id: 22, name: 'LLM generated tokens', unit: 'token' },
  { id: 23, name: 'LLM prefill(ms)', unit: 'ms' },
  { id: 24, name: 'LLM decode(ms)', unit: 'ms' },
];

export class BenchmarkRunner {
  private static _baseDir: string = '';

  static init(filesDir: string): void {
    this._baseDir = filesDir;
  }

  /** 采集当前指标快照 */
  static captureSnapshot(label: string): BenchmarkRecord {
    const record: BenchmarkRecord = {
      id: Date.now().toString(36),
      label,
      timestamp: Date.now(),
      type: 'snapshot',
      metrics: {},
    };
    for (let i: number = 0; i < BENCH_METRICS.length; i++) {
      const m = BENCH_METRICS[i];
      try {
        const s = MetricsCollector.getSummary(m.id) as Record<string, Object>;
        record.metrics[m.id.toString()] = {
          avg: (s['avg'] as number) || 0,
          min: (s['min'] as number) || 0,
          max: (s['max'] as number) || 0,
          p95: (s['p95'] as number) || 0,
          count: (s['count'] as number) || 0,
        };
      } catch (_) {
        record.metrics[m.id.toString()] = { avg: 0, min: 0, max: 0, p95: 0, count: 0 };
      }
    }
    this._save(record);
    return record;
  }

  /** 调用 C++ BenchmarkEngine 跑主动压测 */
  static runNativeBenchmark(label: string): NativeBenchmarkResult {
    const json: string = nativeLib.runBenchmark() as string;
    const result: NativeBenchmarkResult = JSON.parse(json) as NativeBenchmarkResult;

    const record: BenchmarkRecord = {
      id: Date.now().toString(36),
      label,
      timestamp: Date.now(),
      type: 'native',
      metrics: {},
      nativeResult: result,
    };
    this._save(record);
    return result;
  }

  /** 获取所有历史记录 */
  static getHistory(): BenchmarkRecord[] {
    return this._loadAll();
  }

  /** 对比两个快照（按 label 匹配） */
  static compare(baselineLabel: string, currentLabel: string): string {
    const all: BenchmarkRecord[] = this._loadAll();
    let baseline: BenchmarkRecord | null = null;
    let current: BenchmarkRecord | null = null;

    if (baselineLabel || currentLabel) {
      for (let i: number = 0; i < all.length; i++) {
        if (all[i].label === baselineLabel) baseline = all[i];
        if (all[i].label === currentLabel) current = all[i];
      }
    }
    if (!baseline && !current) {
      if (all.length >= 2) { current = all[all.length - 1]; baseline = all[all.length - 2]; }
      else if (all.length === 1) { current = all[0]; }
    }
    if (!current) return '暂无足够基准数据';
    if (!baseline) baseline = current;

    let report = '📊 性能对比\n';
    report += '基线: ' + baseline.label + ' (' + new Date(baseline.timestamp).toLocaleTimeString() + ')\n';
    report += '当前: ' + current.label + ' (' + new Date(current.timestamp).toLocaleTimeString() + ')\n\n';

    const names: string[] = [
      'RTF', 'ASRms', 'VADms', '', 'TTSms', '', '', '', 'LLMms', '', '', '', '',
      'TTFTms', 'Tok/s', 'TTS首包ms', 'TTS RTF', 'TTS队列ms', 'RSS MB', '峰值RSS MB', 'TTS线程'
    ];
    const allKeys = new Set<string>();
    for (const key of Object.keys(baseline.metrics)) allKeys.add(key);
    for (const key of Object.keys(current.metrics)) allKeys.add(key);

    for (const key of allKeys) {
      const b = baseline.metrics[key];
      const c = current.metrics[key];
      if (!b || !c || b.count === 0) continue;
      const kid: number = parseInt(key);
      const name: string = kid >= 0 && kid < names.length && names[kid] ? names[kid] : '#' + key;
      const diff = c.avg - b.avg;
      const arrow = diff <= 0 ? '↓' : '↑';
      const pct = b.avg > 0 ? ((diff / b.avg) * 100).toFixed(1) : '—';
      report += name + ': ' + b.avg.toFixed(3) + ' → ' + c.avg.toFixed(3);
      if (b.avg > 0) report += ' (' + arrow + ' ' + Math.abs(diff).toFixed(3) + ', ' + pct + '%)';
      report += '\n';
    }
    return report;
  }

  /** 对比最近两次 native benchmark */
  static compareLastTwo(): string {
    const all: BenchmarkRecord[] = this._loadAll();
    const natives = all.filter(r => r.type === 'native');
    if (natives.length < 2) return '需要至少 2 次 native benchmark 才能对比';

    const before = natives[natives.length - 2];
    const after = natives[natives.length - 1];
    if (!before.nativeResult || !after.nativeResult) return '数据不完整';

    let report: string = '📊 Benchmark 对比\n';
    report += '基线: ' + before.label + '\n';
    report += '当前: ' + after.label + '\n\n';

    // 内存对比
    const bMem = before.nativeResult.memory;
    const aMem = after.nativeResult.memory;
    const rssDiff = aMem.rss_mb - bMem.rss_mb;
    report += '--- 内存 ---\n';
    report += 'RSS: ' + bMem.rss_mb.toFixed(1) + 'MB → ' + aMem.rss_mb.toFixed(1) + 'MB';
    if (rssDiff !== 0) report += ' (' + (rssDiff > 0 ? '↑' : '↓') + ' ' + Math.abs(rssDiff).toFixed(1) + 'MB)';
    report += '\n';
    report += '线程数: ' + bMem.threads + ' → ' + aMem.threads + '\n\n';

    // Pipeline 对比
    const bPipe = before.nativeResult.pipeline_benchmark;
    const aPipe = after.nativeResult.pipeline_benchmark;
    report += '--- Pipeline (帧大小: ' + bPipe.frame_size + ' samples) ---\n';

    const metrics = [
      { name: '平均延迟', key: 'avg', unit: 'ms', lowerBetter: true },
      { name: 'P95 延迟', key: 'p95', unit: 'ms', lowerBetter: true },
      { name: 'P99 延迟', key: 'p99', unit: 'ms', lowerBetter: true },
    ];
    for (const m of metrics) {
      const bv = (bPipe.latency_ms as any)[m.key] as number;
      const av = (aPipe.latency_ms as any)[m.key] as number;
      const diff = av - bv;
      const arrow = diff <= 0 ? '↓' : '↑';
      const pct = bv > 0 ? ((diff / bv) * 100).toFixed(1) : '—';
      report += m.name + ': ' + bv.toFixed(3) + m.unit + ' → ' + av.toFixed(3) + m.unit;
      if (bv > 0) report += ' (' + arrow + ' ' + Math.abs(diff).toFixed(3) + m.unit + ', ' + pct + '%)';
      report += '\n';
    }

    report += '\n吞吐: ' + bPipe.throughput.frames_per_sec.toFixed(0) + '帧/秒 → '
      + aPipe.throughput.frames_per_sec.toFixed(0) + '帧/秒\n';

    return report;
  }

  static reset(): void {
    this._saveAll([]);
  }

  private static _path(): string {
    return this._baseDir + STORAGE_PATH;
  }

  private static _save(record: BenchmarkRecord): void {
    const all: BenchmarkRecord[] = this._loadAll();
    all.push(record);
    const trimmed: BenchmarkRecord[] = all.slice(-20);
    this._saveAll(trimmed);
  }

  private static _loadAll(): BenchmarkRecord[] {
    try {
      return JSON.parse(fileIo.readTextSync(this._path())) as BenchmarkRecord[];
    } catch (_) { return []; }
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
