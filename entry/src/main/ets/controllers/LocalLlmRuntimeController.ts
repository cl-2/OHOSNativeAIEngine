import nativeLib from 'libnative_lib.so';
import { LlmManager, LlmModelInfo } from '../utils/LlmManager';
import { RuntimeMemoryManager } from '../utils/RuntimeMemoryManager';

const LOCAL_IDLE_RELEASE_MS: number = 60000;

export type LocalLlmModelInfo = LlmModelInfo;

export interface LocalLlmRuntimeCallbacks {
  onStatus: (message: string, state: string) => void;
  appendContext: (userText: string, assistantText: string) => void;
  shouldReleaseAfterRequest: () => boolean;
  onReleased: (state: string) => void;
}

/** Owns the GGUF runtime, active request count and idle-release timer. */
export class LocalLlmRuntimeController {
  private manager: LlmManager = LlmManager.getInstance();
  private activeRequests: number = 0;
  private releaseTimer: number = -1;

  get state(): string { return this.manager.state; }
  get isReady(): boolean { return this.manager.isReady; }
  get isInstalled(): boolean { return this.manager.isInstalled; }
  get activeRequestCount(): number { return this.activeRequests; }

  init(modelDir: string): void { this.manager.init(modelDir); }
  getModelInfo(): LocalLlmModelInfo { return this.manager.getModelInfo(); }
  startupCheck(onStatus?: (message: string) => void): void { this.manager.startupCheck(onStatus); }
  ensureReady(onStatus?: (message: string) => void): Promise<boolean> { return this.manager.ensureReady(onStatus); }
  downloadAndSetup(onProgress?: (pct: number, speed: number) => void,
    onStatus?: (message: string) => void): Promise<boolean> {
    return this.manager.downloadAndSetup(onProgress, onStatus);
  }
  cancelDownload(): void { this.manager.cancelDownload(); }
  verifyInstalledModel(onStatus?: (message: string) => void): Promise<boolean> {
    return this.manager.verifyInstalledModel(onStatus);
  }
  deleteInstalledModel(onStatus?: (message: string) => void): boolean {
    return this.manager.deleteInstalledModel(onStatus);
  }

  cancelIdleRelease(): void {
    if (this.releaseTimer < 0) return;
    clearTimeout(this.releaseTimer);
    this.releaseTimer = -1;
  }

  scheduleIdleRelease(shouldRelease: () => boolean, onReleased: (state: string) => void): void {
    this.cancelIdleRelease();
    if (!this.manager.isReady) return;
    this.releaseTimer = setTimeout((): void => {
      this.releaseTimer = -1;
      if (!shouldRelease()) return;
      if (this.activeRequests > 0) {
        this.scheduleIdleRelease(shouldRelease, onReleased);
        return;
      }
      console.info('OHOS_LLM: releasing idle local runtime after 60s in remote mode');
      this.manager.releaseRuntime();
      onReleased(this.manager.state);
      RuntimeMemoryManager.capture('LLM空闲释放后');
    }, LOCAL_IDLE_RELEASE_MS);
  }

  releaseIfIdle(force: boolean = false): boolean {
    if (this.activeRequests > 0) return false;
    this.cancelIdleRelease();
    this.manager.releaseRuntime(force);
    return true;
  }

  releaseRuntime(force: boolean = false): void {
    this.cancelIdleRelease();
    this.manager.releaseRuntime(force);
  }

  generate(
    text: string,
    historyJson: string,
    onToken: (token: string) => void,
    onComplete: (fullText: string) => void,
    callbacks: LocalLlmRuntimeCallbacks
  ): void {
    this.cancelIdleRelease();
    this.manager.ensureReady((message: string): void => {
      callbacks.onStatus(message, this.manager.state);
    }).then((ready: boolean): void => {
      callbacks.onStatus('', this.manager.state);
      if (!ready) {
        console.error('OHOS_LLM_LOCAL: runtime unavailable');
        onComplete('');
        return;
      }
      RuntimeMemoryManager.capture('本地LLM加载后');
      console.info('OHOS_LLM_LOCAL: calling local LLM, text="' + text.substring(0, 50) + '"');
      let settled: boolean = false;
      this.activeRequests++;
      const finish = (resultText: string, success: boolean): void => {
        if (settled) return;
        settled = true;
        this.activeRequests = Math.max(0, this.activeRequests - 1);
        console.info('OHOS_LLM_LOCAL: done, success=' + success + ' len=' + resultText.length);
        if (success && resultText) callbacks.appendContext(text, resultText);
        RuntimeMemoryManager.capture('本地LLM生成完成');
        onComplete(success ? resultText : '');
        if (callbacks.shouldReleaseAfterRequest()) {
          this.scheduleIdleRelease(callbacks.shouldReleaseAfterRequest, callbacks.onReleased);
        }
      };
      try {
        nativeLib.callLocalLlm(text, historyJson,
          (token: string): void => onToken(token),
          (resultText: string, success: boolean): void => finish(resultText, success));
      } catch (error) {
        console.error('OHOS_LLM_LOCAL: error=' + String(error));
        finish('', false);
      }
    }).catch((error: Error): void => {
      console.error('OHOS_LLM_LOCAL: lazy load failed=' + String(error));
      onComplete('');
    });
  }
}
