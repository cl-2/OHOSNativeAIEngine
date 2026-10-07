/**
 * Repeatable on-device memory scenario benchmark.
 * It deliberately exercises only local inference/TTS. Real microphone barge-in
 * still needs a human test because AEC depends on the physical acoustic path.
 */
import { fileIo } from '@kit.CoreFileKit';
import nativeLib from 'libnative_lib.so';
import { LlmManager } from './LlmManager';
import { RuntimeMemoryManager, RuntimeMemorySnapshot } from './RuntimeMemoryManager';

export interface MemorySample {
  stage: string;
  rssMedianMb: number;
  rssMinMb: number;
  rssMaxMb: number;
  peakMedianMb: number;
  threadsMedian: number;
  samples: number;
  engines: string;
}

export interface LocalGenerationResult {
  run: number;
  totalMs: number;
  firstTokenMs: number;
  textLength: number;
  tokenCallbacks: number;
}

export interface TtsGenerationResult {
  run: number;
  totalMs: number;
  firstChunkMs: number;
}

export interface MemoryScenarioReport {
  schemaVersion: number;
  startedAt: number;
  finishedAt: number;
  success: boolean;
  error: string;
  samples: MemorySample[];
  localLoadMs: number;
  localRuns: LocalGenerationResult[];
  ttsRuns: TtsGenerationResult[];
}

const SAMPLE_COUNT: number = 5;
const SAMPLE_INTERVAL_MS: number = 400;
const SETTLE_MS: number = 1500;
const DIAGNOSTIC_DECODE_TOKENS: number = 96;
const LOCAL_PROMPT: string = '只回复“测试完成”，不要解释，不要添加标点。';
const TTS_TEXT: string = '这是端侧语音助手内存场景测试。请保持自然平稳的语速完成朗读。';

export class MemoryScenarioRunner {
  private static baseDir: string = '';
  private static running: boolean = false;

  static init(filesDir: string): void {
    this.baseDir = filesDir;
  }

  static get isRunning(): boolean {
    return this.running;
  }

  static getHistory(): MemoryScenarioReport[] {
    if (!this.baseDir) return [];
    return this.loadHistory(this.baseDir + '/perf/memory_scenarios.json');
  }

  static async run(onStatus?: (status: string) => void): Promise<MemoryScenarioReport> {
    if (this.running) throw new Error('内存场景压测正在运行');
    if (!this.baseDir) throw new Error('内存场景压测尚未初始化');
    const manager: LlmManager = LlmManager.getInstance();
    if (!manager.isInstalled) throw new Error('请先下载并校验本地 GGUF 模型');

    this.running = true;
    const report: MemoryScenarioReport = {
      schemaVersion: 1,
      startedAt: Date.now(),
      finishedAt: 0,
      success: false,
      error: '',
      samples: [],
      localLoadMs: 0,
      localRuns: [],
      ttsRuns: [],
    };
    let llmLease: boolean = false;
    let ttsLease: boolean = false;
    try {
      onStatus?.('准备基线：释放空闲 LLM/TTS…');
      manager.releaseRuntime();
      try { nativeLib.releaseTtsEngine(); } catch (_) {}
      await this.sleep(SETTLE_MS);
      report.samples.push(await this.sampleMedian('ASR常驻基线'));

      onStatus?.('加载本地 LLM…');
      manager.beginRuntimeLease();
      llmLease = true;
      const loadStart: number = Date.now();
      const ready: boolean = await manager.ensureReady((message: string): void => onStatus?.(message));
      report.localLoadMs = Date.now() - loadStart;
      if (!ready) throw new Error('本地 LLM 加载失败');
      await this.sleep(SETTLE_MS);
      report.samples.push(await this.sampleMedian('本地LLM加载后'));

      for (let i: number = 0; i < 3; i++) {
        onStatus?.('本地 LLM 固定提示词 ' + (i + 1) + '/3…');
        report.localRuns.push(await this.runLocalGeneration(i + 1));
        await this.sleep(500);
      }
      report.samples.push(await this.sampleMedian('本地LLM三轮生成后'));

      manager.endRuntimeLease();
      llmLease = false;
      manager.releaseRuntime();
      await this.sleep(SETTLE_MS);
      report.samples.push(await this.sampleMedian('本地LLM释放后'));

      onStatus?.('加载 TTS 并执行固定文本…');
      nativeLib.beginTtsLifecycleLease();
      ttsLease = true;
      for (let i: number = 0; i < 2; i++) {
        report.ttsRuns.push(await this.runTtsGeneration(i + 1));
        await this.sleep(500);
      }
      await this.sleep(SETTLE_MS);
      report.samples.push(await this.sampleMedian('TTS热态生成后'));

      nativeLib.endTtsLifecycleLease();
      ttsLease = false;
      nativeLib.releaseTtsEngine();
      await this.sleep(SETTLE_MS);
      report.samples.push(await this.sampleMedian('TTS释放后'));
      report.success = true;
      onStatus?.('内存场景压测完成');
    } catch (e) {
      report.error = String(e);
      onStatus?.('压测失败：' + report.error);
    } finally {
      if (ttsLease) {
        try { nativeLib.endTtsLifecycleLease(); } catch (_) {}
      }
      if (llmLease) manager.endRuntimeLease();
      report.finishedAt = Date.now();
      this.persist(report);
      this.running = false;
    }
    return report;
  }

  private static async sampleMedian(stage: string): Promise<MemorySample> {
    const points: RuntimeMemorySnapshot[] = [];
    for (let i: number = 0; i < SAMPLE_COUNT; i++) {
      const snapshot: RuntimeMemorySnapshot | undefined = RuntimeMemoryManager.readCurrent(stage);
      if (snapshot) points.push(snapshot);
      if (i + 1 < SAMPLE_COUNT) await this.sleep(SAMPLE_INTERVAL_MS);
    }
    if (points.length === 0) throw new Error(stage + ' 无法读取 RSS');
    const rss: number[] = points.map((p: RuntimeMemorySnapshot): number => p.rssMb).sort((a, b) => a - b);
    const peak: number[] = points.map((p: RuntimeMemorySnapshot): number => p.peakMb).sort((a, b) => a - b);
    const threads: number[] = points.map((p: RuntimeMemorySnapshot): number => p.threads).sort((a, b) => a - b);
    const middle: RuntimeMemorySnapshot = points[Math.floor(points.length / 2)];
    RuntimeMemoryManager.capture(stage, true);
    return {
      stage,
      rssMedianMb: this.median(rss),
      rssMinMb: rss[0],
      rssMaxMb: rss[rss.length - 1],
      peakMedianMb: this.median(peak),
      threadsMedian: this.median(threads),
      samples: points.length,
      engines: (middle.asrLoaded ? 'A' : '-') + (middle.ttsLoaded ? 'T' : '-') +
        (middle.llmLoaded ? 'L' : '-'),
    };
  }

  private static runLocalGeneration(run: number): Promise<LocalGenerationResult> {
    return new Promise<LocalGenerationResult>((resolve, reject): void => {
      const start: number = Date.now();
      let firstTokenMs: number = 0;
      let tokenCallbacks: number = 0;
      let settled: boolean = false;
      const finish = (result: LocalGenerationResult | undefined, error: string): void => {
        if (settled) return;
        settled = true;
        if (result) resolve(result); else reject(new Error(error));
      };
      try {
        const applied: number = nativeLib.setLocalLlmDiagnosticTokens(DIAGNOSTIC_DECODE_TOKENS);
        if (applied !== DIAGNOSTIC_DECODE_TOKENS) {
          finish(undefined, '无法启用固定 LLM 诊断负载');
          return;
        }
        nativeLib.callLocalLlm(
          LOCAL_PROMPT,
          '[{"role":"system","content":"你是性能测试助手，必须严格按用户要求回答。"}]',
          (token: string): void => {
            if (token) {
              tokenCallbacks++;
              if (!firstTokenMs) firstTokenMs = Date.now() - start;
            }
          },
          (text: string, success: boolean): void => {
            if (!success) {
              finish(undefined, '本地 LLM 第 ' + run + ' 轮生成失败');
              return;
            }
            finish({
              run,
              totalMs: Date.now() - start,
              firstTokenMs,
              textLength: text.length,
              tokenCallbacks,
            }, '');
          }
        );
      } catch (e) {
        finish(undefined, String(e));
      }
    });
  }

  private static runTtsGeneration(run: number): Promise<TtsGenerationResult> {
    return new Promise<TtsGenerationResult>((resolve, reject): void => {
      const start: number = Date.now();
      let firstChunkMs: number = 0;
      let settled: boolean = false;
      const finish = (result: TtsGenerationResult | undefined, error: string): void => {
        if (settled) return;
        settled = true;
        if (result) resolve(result); else reject(new Error(error));
      };
      try {
        nativeLib.startStreamingTtsWithSpeed(
          TTS_TEXT,
          this.baseDir + '/models/tts',
          1.0,
          (_data: ArrayBuffer, progress: number, _sampleRate: number, generation: number,
            sampleCount?: number): void => {
            // Do not read properties from _data here. On some HarmonyOS Ark
            // runtimes a direct NAPI callback receives a native wrapper rather
            // than an ECMA ArrayBuffer, and data.byteLength throws TypeError.
            const count: number = sampleCount || 0;
            if (count > 0) {
              if (!firstChunkMs) firstChunkMs = Date.now() - start;
              nativeLib.ackTtsChunk(count, generation);
            }
            if (progress >= 2.0) {
              finish({ run, totalMs: Date.now() - start, firstChunkMs }, '');
            } else if (progress < 0) {
              finish(undefined, 'TTS 第 ' + run + ' 轮生成失败');
            }
          }
        );
      } catch (e) {
        finish(undefined, String(e));
      }
    });
  }

  private static median(values: number[]): number {
    return values[Math.floor(values.length / 2)];
  }

  private static sleep(ms: number): Promise<void> {
    return new Promise<void>((resolve: Function): void => {
      setTimeout((): void => { resolve(); }, ms);
    });
  }

  private static persist(report: MemoryScenarioReport): void {
    try {
      const perfDir: string = this.baseDir + '/perf';
      fileIo.mkdirSync(perfDir, true);
      const path: string = perfDir + '/memory_scenarios.json';
      const temp: string = path + '.tmp';
      const history: MemoryScenarioReport[] = this.loadHistory(path);
      history.push(report);
      const file: fileIo.File = fileIo.openSync(
        temp,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC
      );
      fileIo.writeSync(file.fd, JSON.stringify(history.slice(-10)));
      fileIo.closeSync(file);
      try { fileIo.unlinkSync(path); } catch (_) {}
      fileIo.renameSync(temp, path);
    } catch (e) {
      console.warn('OHOS_MEMORY_SCENARIO: persist failed=' + String(e));
    }
  }

  private static loadHistory(path: string): MemoryScenarioReport[] {
    try { return JSON.parse(fileIo.readTextSync(path)) as MemoryScenarioReport[]; } catch (_) { return []; }
  }
}
