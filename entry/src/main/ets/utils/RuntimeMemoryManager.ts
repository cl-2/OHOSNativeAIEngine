import { fileIo } from '@kit.CoreFileKit';
import nativeLib from 'libnative_lib.so';

export interface RuntimeMemorySnapshot {
  timestamp: number;
  stage: string;
  rssMb: number;
  peakMb: number;
  threads: number;
  asrLoaded: boolean;
  asrRunning: boolean;
  vadLoaded: boolean;
  ttsLoaded: boolean;
  llmLoaded: boolean;
}

export interface ModelFootprint {
  asrMb: number;
  vadMb: number;
  ttsMb: number;
  llmMb: number;
  totalMb: number;
}

interface NativeMemorySnapshot {
  rssKb: number;
  peakKb: number;
  threads: number;
  asrLoaded: boolean;
  asrRunning: boolean;
  vadLoaded: boolean;
  ttsLoaded: boolean;
  llmLoaded: boolean;
}

export class RuntimeMemoryManager {
  private static baseDir: string = '';
  private static snapshots: RuntimeMemorySnapshot[] = [];
  private static pendingMemoryLevel: number = -1;
  private static backgroundRequested: boolean = false;
  private static foregroundRequested: boolean = false;
  private static lastStage: string = '';
  private static lastCaptureAt: number = 0;

  static init(filesDir: string): void {
    this.baseDir = filesDir;
    // A diagnostics file represents one app process. Old snapshots remain in
    // previously exported diagnostics and must not leak into the new session.
    this.snapshots = [];
    this.lastStage = '';
    this.lastCaptureAt = 0;
  }

  static capture(stage: string, force: boolean = false): RuntimeMemorySnapshot | undefined {
    const now: number = Date.now();
    if (!force && stage === this.lastStage && now - this.lastCaptureAt < 1000) {
      return this.snapshots.length > 0 ? this.snapshots[this.snapshots.length - 1] : undefined;
    }
    const snapshot: RuntimeMemorySnapshot | undefined = this.readCurrent(stage);
    if (!snapshot) return undefined;
    this.lastStage = stage;
    this.lastCaptureAt = now;
    this.snapshots.push(snapshot);
    this.snapshots = this.snapshots.slice(-40);
    this.persist();
    console.info('OHOS_MEMORY: stage=' + stage + ' rss=' + snapshot.rssMb.toFixed(1) +
      'MB asr=' + snapshot.asrLoaded + ' tts=' + snapshot.ttsLoaded +
      ' llm=' + snapshot.llmLoaded);
    return snapshot;
  }

  /** Reads an RSS point without adding it to the visible stage history. */
  static readCurrent(stage: string): RuntimeMemorySnapshot | undefined {
    try {
      const raw: NativeMemorySnapshot =
        JSON.parse(nativeLib.getRuntimeMemorySnapshot()) as NativeMemorySnapshot;
      return {
        timestamp: Date.now(),
        stage,
        rssMb: raw.rssKb / 1024,
        peakMb: raw.peakKb / 1024,
        threads: raw.threads,
        asrLoaded: raw.asrLoaded,
        asrRunning: raw.asrRunning,
        vadLoaded: raw.vadLoaded,
        ttsLoaded: raw.ttsLoaded,
        llmLoaded: raw.llmLoaded,
      };
    } catch (e) {
      console.warn('OHOS_MEMORY: read snapshot failed=' + String(e));
      return undefined;
    }
  }

  static getRecentSnapshots(): RuntimeMemorySnapshot[] {
    return this.snapshots.slice();
  }

  static reportMemoryPressure(level: number): void {
    this.pendingMemoryLevel = Math.max(this.pendingMemoryLevel, level);
    console.warn('OHOS_MEMORY: system pressure level=' + level);
  }

  static consumeMemoryPressure(): number {
    const level: number = this.pendingMemoryLevel;
    this.pendingMemoryLevel = -1;
    return level;
  }

  static reportAppBackground(): void {
    this.backgroundRequested = true;
  }

  static consumeAppBackground(): boolean {
    const requested: boolean = this.backgroundRequested;
    this.backgroundRequested = false;
    return requested;
  }

  static reportAppForeground(): void {
    this.foregroundRequested = true;
  }

  static consumeAppForeground(): boolean {
    const requested: boolean = this.foregroundRequested;
    this.foregroundRequested = false;
    return requested;
  }

  static getModelFootprint(): ModelFootprint {
    const modelDir: string = this.baseDir + '/models';
    const asrBytes: number = this.sumFiles([
      modelDir + '/asr/encoder-epoch-99-avg-1.int8.onnx',
      modelDir + '/asr/decoder-epoch-99-avg-1.int8.onnx',
      modelDir + '/asr/joiner-epoch-99-avg-1.onnx',
      modelDir + '/asr/tokens.txt',
    ]);
    const vadBytes: number = this.sumFiles([modelDir + '/vad/silero_vad.onnx']);
    const ttsBytes: number = this.sumFiles([
      modelDir + '/tts/model.onnx',
      modelDir + '/tts/tokens.txt',
      modelDir + '/tts/lexicon.txt',
    ]);
    const llmBytes: number = this.sumFiles([modelDir + '/llm/model.gguf']);
    const divisor: number = 1024 * 1024;
    return {
      asrMb: asrBytes / divisor,
      vadMb: vadBytes / divisor,
      ttsMb: ttsBytes / divisor,
      llmMb: llmBytes / divisor,
      totalMb: (asrBytes + vadBytes + ttsBytes + llmBytes) / divisor,
    };
  }

  private static sumFiles(paths: string[]): number {
    let total: number = 0;
    for (let i: number = 0; i < paths.length; i++) {
      try { total += fileIo.statSync(paths[i]).size; } catch (_) {}
    }
    return total;
  }

  private static snapshotPath(): string {
    return this.baseDir + '/perf/memory_stages.json';
  }

  private static pathExists(path: string): boolean {
    try {
      return fileIo.accessSync(path);
    } catch (e) {
      const message: string = String(e).toLowerCase();
      if (message.includes('no such file') || message.includes('13900002')) return false;
      throw e;
    }
  }

  private static persist(): void {
    if (!this.baseDir) return;
    try {
      const perfDir: string = this.baseDir + '/perf';
      if (!this.pathExists(perfDir)) fileIo.mkdirSync(perfDir);
      const path: string = this.snapshotPath();
      let mode: number = fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC;
      if (!this.pathExists(path)) mode |= fileIo.OpenMode.CREATE;
      const file: fileIo.File = fileIo.openSync(
        path,
        mode
      );
      fileIo.writeSync(file.fd, JSON.stringify(this.snapshots));
      fileIo.closeSync(file);
    } catch (e) {
      console.warn('OHOS_MEMORY: persist failed=' + String(e));
    }
  }
}
