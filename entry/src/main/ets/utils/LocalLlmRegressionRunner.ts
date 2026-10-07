import { fileIo } from '@kit.CoreFileKit';
import nativeLib from 'libnative_lib.so';
import { LlmManager } from './LlmManager';

export interface LocalLlmRegressionCase {
  name: string;
  expected: string;
  responsePreview: string;
  passed: boolean;
  detail: string;
  firstTokenMs: number;
  totalMs: number;
  tokenCallbacks: number;
}

export interface LocalLlmRegressionReport {
  schemaVersion: number;
  startedAt: number;
  finishedAt: number;
  success: boolean;
  passed: number;
  failed: number;
  duplicateResponses: number;
  cases: LocalLlmRegressionCase[];
  error: string;
  scope: string;
  persisted: boolean;
  persistError: string;
}

interface RegressionVector {
  name: string;
  prompt: string;
  expected: string;
}

interface GenerationObservation {
  vector: RegressionVector;
  text: string;
  firstTokenMs: number;
  totalMs: number;
  tokenCallbacks: number;
  success: boolean;
}

const MAX_GENERATED_TOKENS: number = 160;
const CALL_TIMEOUT_MS: number = 45000;
const HISTORY_JSON: string = JSON.stringify([
  { role: 'system', content: '你是端侧LLM回归助手。用户要求输出测试代号时，必须只输出该代号。' },
  { role: 'user', content: '请只输出测试代号 OLD1。' },
  { role: 'assistant', content: 'OLD1' },
  { role: 'user', content: '请只输出测试代号 OLD2。' },
  { role: 'assistant', content: 'OLD2' },
]);

const VECTORS: RegressionVector[] = [
  { name: 'parallel_callback_A17', prompt: '请严格只输出测试代号 A17，不要解释。', expected: 'A17' },
  { name: 'parallel_callback_B29', prompt: '请严格只输出测试代号 B29，不要解释。', expected: 'B29' },
  { name: 'sequential_prompt_C43', prompt: '忽略旧代号，只输出新的测试代号 C43。', expected: 'C43' },
  { name: 'sequential_prompt_D61', prompt: '忽略旧代号，只输出新的测试代号 D61。', expected: 'D61' },
];

export class LocalLlmRegressionRunner {
  private static baseDir: string = '';
  private static historyBaseDir: string = '';
  private static history: LocalLlmRegressionReport[] = [];
  private static running: boolean = false;

  static init(filesDir: string): void {
    this.baseDir = filesDir;
    if (this.historyBaseDir !== filesDir) {
      this.historyBaseDir = filesDir;
      this.history = this.loadHistory(filesDir + '/perf/local_llm_regressions.json');
    }
  }

  static getHistory(): LocalLlmRegressionReport[] {
    return this.history.slice();
  }

  static async run(onStatus?: (status: string) => void): Promise<LocalLlmRegressionReport> {
    if (this.running) throw new Error('本地 LLM 端到端回归正在运行');
    if (!this.baseDir) throw new Error('本地 LLM 端到端回归尚未初始化');
    this.running = true;
    const report: LocalLlmRegressionReport = {
      schemaVersion: 1,
      startedAt: Date.now(),
      finishedAt: 0,
      success: false,
      passed: 0,
      failed: 0,
      duplicateResponses: 0,
      cases: [],
      error: '',
      scope: '真实GGUF：并发回调归属、多问题响应隔离、重复回复、160-token输出上限',
      persisted: false,
      persistError: '',
    };
    const manager: LlmManager = LlmManager.getInstance();
    let leased: boolean = false;
    try {
      manager.beginRuntimeLease();
      leased = true;
      onStatus?.('正在准备本地 GGUF 模型…');
      const ready: boolean = await manager.ensureReady(onStatus);
      if (!ready) throw new Error('本地 GGUF 未安装或加载失败');

      onStatus?.('回归 1/2：并发请求与回调隔离…');
      const parallel: GenerationObservation[] = await Promise.all([
        this.runGeneration(VECTORS[0]),
        this.runGeneration(VECTORS[1]),
      ]);
      const observations: GenerationObservation[] = [parallel[0], parallel[1]];

      for (let i: number = 2; i < VECTORS.length; i++) {
        onStatus?.('回归 2/2：多问题串行 ' + (i - 1) + '/' + (VECTORS.length - 2) + '…');
        observations.push(await this.runGeneration(VECTORS[i]));
      }
      report.cases = this.evaluate(observations, report);
      for (let i: number = 0; i < report.cases.length; i++) {
        if (report.cases[i].passed) report.passed++; else report.failed++;
      }
      report.success = report.failed === 0;
      onStatus?.(report.success ? '真实 GGUF 回归通过' : '真实 GGUF 回归发现异常');
    } catch (e) {
      report.error = String(e);
      report.failed++;
      onStatus?.('真实 GGUF 回归失败：' + report.error);
    } finally {
      if (leased) manager.endRuntimeLease();
      report.finishedAt = Date.now();
      this.persist(report);
      this.running = false;
    }
    return report;
  }

  private static runGeneration(vector: RegressionVector): Promise<GenerationObservation> {
    return new Promise<GenerationObservation>((resolve): void => {
      const startedAt: number = Date.now();
      let firstTokenMs: number = 0;
      let tokenCallbacks: number = 0;
      let settled: boolean = false;
      const timeoutId: number = setTimeout((): void => {
        if (settled) return;
        settled = true;
        resolve({
          vector,
          text: '',
          firstTokenMs,
          totalMs: Date.now() - startedAt,
          tokenCallbacks,
          success: false,
        });
      }, CALL_TIMEOUT_MS);
      try {
        nativeLib.callLocalLlm(
          vector.prompt,
          HISTORY_JSON,
          (token: string): void => {
            if (settled || !token) return;
            tokenCallbacks++;
            if (!firstTokenMs) firstTokenMs = Date.now() - startedAt;
          },
          (text: string, success: boolean): void => {
            if (settled) return;
            settled = true;
            clearTimeout(timeoutId);
            resolve({
              vector,
              text,
              firstTokenMs,
              totalMs: Date.now() - startedAt,
              tokenCallbacks,
              success,
            });
          }
        );
      } catch (_) {
        if (!settled) {
          settled = true;
          clearTimeout(timeoutId);
          resolve({
            vector,
            text: '',
            firstTokenMs,
            totalMs: Date.now() - startedAt,
            tokenCallbacks,
            success: false,
          });
        }
      }
    });
  }

  private static normalize(text: string): string {
    return text.toUpperCase().replace(/[^A-Z0-9]/g, '');
  }

  private static evaluate(observations: GenerationObservation[],
    report: LocalLlmRegressionReport): LocalLlmRegressionCase[] {
    const normalized: string[] = [];
    for (let i: number = 0; i < observations.length; i++) {
      normalized.push(this.normalize(observations[i].text));
    }
    const duplicateIndexes: boolean[] = new Array<boolean>(observations.length).fill(false);
    for (let i: number = 0; i < normalized.length; i++) {
      if (!normalized[i]) continue;
      for (let j: number = i + 1; j < normalized.length; j++) {
        if (normalized[i] === normalized[j]) {
          duplicateIndexes[i] = true;
          duplicateIndexes[j] = true;
          report.duplicateResponses++;
        }
      }
    }

    const cases: LocalLlmRegressionCase[] = [];
    for (let i: number = 0; i < observations.length; i++) {
      const item: GenerationObservation = observations[i];
      const expectedFound: boolean = normalized[i].includes(item.vector.expected);
      const withinLimit: boolean = item.tokenCallbacks <= MAX_GENERATED_TOKENS;
      const callbackHealthy: boolean = item.firstTokenMs > 0 && item.tokenCallbacks > 0;
      const passed: boolean = item.success && expectedFound && withinLimit &&
        callbackHealthy && !duplicateIndexes[i];
      const detail: string = 'expected=' + item.vector.expected +
        ',found=' + expectedFound + ',callbacks=' + item.tokenCallbacks +
        ',first=' + item.firstTokenMs + 'ms,total=' + item.totalMs +
        'ms,duplicate=' + duplicateIndexes[i] + ',success=' + item.success;
      cases.push({
        name: item.vector.name,
        expected: item.vector.expected,
        responsePreview: item.text.substring(0, 80),
        passed,
        detail,
        firstTokenMs: item.firstTokenMs,
        totalMs: item.totalMs,
        tokenCallbacks: item.tokenCallbacks,
      });
    }
    return cases;
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

  private static persist(report: LocalLlmRegressionReport): void {
    this.history.push(report);
    if (this.history.length > 20) this.history = this.history.slice(-20);
    const perfDir: string = this.baseDir + '/perf';
    const path: string = perfDir + '/local_llm_regressions.json';
    try {
      if (!this.pathExists(perfDir)) fileIo.mkdirSync(perfDir);
      report.persisted = true;
      report.persistError = '';
      let mode: number = fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC;
      if (!this.pathExists(path)) mode |= fileIo.OpenMode.CREATE;
      const file: fileIo.File = fileIo.openSync(path, mode);
      fileIo.writeSync(file.fd, JSON.stringify(this.history));
      fileIo.closeSync(file);
    } catch (e) {
      report.persisted = false;
      report.persistError = String(e);
    }
  }

  private static loadHistory(path: string): LocalLlmRegressionReport[] {
    try {
      return JSON.parse(fileIo.readTextSync(path)) as LocalLlmRegressionReport[];
    } catch (_) {
      return [];
    }
  }
}
