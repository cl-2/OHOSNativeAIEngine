/**
 * Local GGUF lifecycle, integrity and installation manager.
 */

import { fileIo } from '@kit.CoreFileKit';
import worker, { MessageEvents } from '@ohos.worker';
import { DownloadState, ModelDownloader } from './ModelDownloader';
import nativeLib from 'libnative_lib.so';

export enum LlmState {
  UNAVAILABLE = 'UNAVAILABLE',
  INSTALLED = 'INSTALLED',
  DOWNLOADING = 'DOWNLOADING',
  VERIFYING = 'VERIFYING',
  READY = 'READY',
  ERROR = 'ERROR',
}

export interface LlmModelInfo {
  filePresent: boolean;
  installed: boolean;
  loaded: boolean;
  sizeBytes: number;
  source: string;
  integrity: string;
  verifiedAt: string;
}

interface LlmModelSource {
  name: string;
  url: string;
  size: number;
  sha256: string;
}

interface LlmReadyWaiter {
  resolve: (ok: boolean) => void;
  onStatus?: (msg: string) => void;
}

interface LlmManifest {
  schemaVersion: number;
  modelId: string;
  source: string;
  size: number;
  sha256: string;
  actualSha256: string;
  verified: boolean;
  verifiedAt: string;
}

const MODEL_ID: string = 'Qwen2.5-0.5B-Instruct-Q4_K_M';
const MODEL_SOURCES: LlmModelSource[] = [
  {
    name: 'ModelScope',
    url: 'https://www.modelscope.cn/models/second-state/Qwen2.5-0.5B-Instruct-GGUF/resolve/master/Qwen2.5-0.5B-Instruct-Q4_K_M.gguf',
    size: 397_807_936,
    sha256: '750f8f144f0504208add7897f01c7d2350a7363d8855eab59e137a1041e90394',
  },
  {
    name: 'Hugging Face',
    url: 'https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/9217f5db79a29953eb74d5343926648285ec7e67/qwen2.5-0.5b-instruct-q4_k_m.gguf',
    size: 491_400_032,
    sha256: '74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db',
  },
];

export class LlmManager {
  private static instance: LlmManager;
  private _state: LlmState = LlmState.UNAVAILABLE;
  private _prewarming: boolean = false;
  private _verifying: boolean = false;
  private _integrityFailed: boolean = false;
  private _loadFailureVerified: boolean = false;
  // Long-running diagnostic work holds this lease so the normal 60-second
  // remote-mode release timer cannot unload the model mid-generation.
  private _runtimeLeaseCount: number = 0;
  private llmWorker?: worker.ThreadWorker;
  private downloader: ModelDownloader = new ModelDownloader();
  private _modelDir: string = '';
  private _readyWaiters: LlmReadyWaiter[] = [];

  static getInstance(): LlmManager {
    if (!this.instance) this.instance = new LlmManager();
    return this.instance;
  }

  init(modelDir: string): void {
    this._modelDir = modelDir;
    this.refreshIntegrityState();
  }

  get state(): LlmState {
    return this._state;
  }

  get isReady(): boolean {
    return this._state === LlmState.READY;
  }

  get isInstalled(): boolean {
    return this.checkModelSync();
  }

  get isDownloading(): boolean {
    return this._state === LlmState.DOWNLOADING;
  }

  get isVerifying(): boolean {
    return this._verifying;
  }

  beginRuntimeLease(): void {
    this._runtimeLeaseCount++;
  }

  endRuntimeLease(): void {
    this._runtimeLeaseCount = Math.max(0, this._runtimeLeaseCount - 1);
  }

  private get modelPath(): string {
    return this._modelDir + '/model.gguf';
  }

  private get manifestPath(): string {
    return this._modelDir + '/model.manifest.json';
  }

  private getFileSize(path: string): number {
    try {
      return fileIo.statSync(path).size;
    } catch (_) {
      return 0;
    }
  }

  private sourceForSize(size: number): LlmModelSource | undefined {
    for (let i: number = 0; i < MODEL_SOURCES.length; i++) {
      if (MODEL_SOURCES[i].size === size) return MODEL_SOURCES[i];
    }
    return undefined;
  }

  private readManifest(): LlmManifest | undefined {
    try {
      const content: string = fileIo.readTextSync(this.manifestPath);
      const manifest: LlmManifest = JSON.parse(content) as LlmManifest;
      if (manifest && manifest.schemaVersion === 1 && manifest.modelId === MODEL_ID) {
        return manifest;
      }
    } catch (_) {}
    return undefined;
  }

  private writeManifest(source: LlmModelSource, verified: boolean, actualSha256: string): void {
    try {
      fileIo.mkdirSync(this._modelDir, true);
    } catch (_) {}
    const manifest: LlmManifest = {
      schemaVersion: 1,
      modelId: MODEL_ID,
      source: source.name,
      size: source.size,
      sha256: source.sha256,
      actualSha256,
      verified,
      verifiedAt: new Date().toISOString(),
    };
    const tempPath: string = this.manifestPath + '.tmp';
    try {
      const file: fileIo.File = fileIo.openSync(
        tempPath,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC
      );
      fileIo.writeSync(file.fd, JSON.stringify(manifest));
      fileIo.closeSync(file);
      try { fileIo.unlinkSync(this.manifestPath); } catch (_) {}
      fileIo.renameSync(tempPath, this.manifestPath);
    } catch (e) {
      console.warn('OHOS_LLM: manifest write failed=' + String(e));
      try { fileIo.unlinkSync(tempPath); } catch (_) {}
    }
  }

  private refreshIntegrityState(): void {
    this._integrityFailed = false;
    const size: number = this.getFileSize(this.modelPath);
    const manifest: LlmManifest | undefined = this.readManifest();
    if (manifest && !manifest.verified && manifest.size === size) {
      this._integrityFailed = true;
    }
  }

  checkModelSync(): boolean {
    if (!this._modelDir || this._integrityFailed) return false;
    return this.sourceForSize(this.getFileSize(this.modelPath)) !== undefined;
  }

  getModelInfo(): LlmModelInfo {
    const size: number = this.getFileSize(this.modelPath);
    const source: LlmModelSource | undefined = this.sourceForSize(size);
    const manifest: LlmManifest | undefined = this.readManifest();
    let integrity: string = '未校验';
    let verifiedAt: string = '';
    let sourceName: string = source ? source.name : '';
    if (manifest && manifest.size === size) {
      integrity = manifest.verified ? '已校验' : '校验失败';
      verifiedAt = manifest.verifiedAt;
      sourceName = manifest.source || sourceName;
    } else if (size > 0 && !source) {
      integrity = '文件异常';
    }
    return {
      filePresent: size > 0,
      installed: !!source && !this._integrityFailed,
      loaded: this._state === LlmState.READY,
      sizeBytes: size,
      source: sourceName,
      integrity,
      verifiedAt,
    };
  }

  /** Fast startup check: recognized size plus the persisted verification manifest. */
  startupCheck(onStatus?: (msg: string) => void): void {
    if (!this._modelDir) {
      onStatus?.('LlmManager 未初始化');
      return;
    }
    this.refreshIntegrityState();
    const info: LlmModelInfo = this.getModelInfo();
    if (info.installed) {
      this._state = LlmState.INSTALLED;
      onStatus?.(info.integrity === '已校验'
        ? '本地模型已安装（需要时加载）'
        : '本地模型已安装（建议执行一次校验）');
    } else if (info.filePresent) {
      this._state = LlmState.ERROR;
      onStatus?.('本地模型文件异常，请重新下载');
    } else {
      this._state = LlmState.UNAVAILABLE;
      onStatus?.('本地模型未安装');
    }
  }

  ensureReady(onStatus?: (msg: string) => void): Promise<boolean> {
    if (this._state === LlmState.READY) return Promise.resolve(true);
    if (!this.checkModelSync()) {
      this._state = this.getFileSize(this.modelPath) > 0 ? LlmState.ERROR : LlmState.UNAVAILABLE;
      onStatus?.(this._state === LlmState.ERROR ? '本地模型文件异常' : '本地模型未安装');
      return Promise.resolve(false);
    }

    this._state = LlmState.INSTALLED;
    return new Promise<boolean>((resolve) => {
      this._readyWaiters.push({ resolve, onStatus });
      if (this._prewarming) {
        onStatus?.('正在加载本地模型…');
        return;
      }
      this.prewarmInWorker();
    });
  }

  private notifyReadyWaiters(status: string): void {
    for (let i: number = 0; i < this._readyWaiters.length; i++) {
      this._readyWaiters[i].onStatus?.(status);
    }
  }

  private finishPrewarm(ok: boolean, status: string): void {
    this._state = ok ? LlmState.READY :
      (this._integrityFailed ? LlmState.ERROR : LlmState.INSTALLED);
    this._prewarming = false;
    const waiters: LlmReadyWaiter[] = this._readyWaiters;
    this._readyWaiters = [];
    for (let i: number = 0; i < waiters.length; i++) {
      waiters[i].onStatus?.(status);
      waiters[i].resolve(ok);
    }
  }

  private verifyAfterLoadFailure(): void {
    if (this._loadFailureVerified) {
      this.finishPrewarm(false, '模型文件完整，但本地引擎加载失败');
      return;
    }
    this._loadFailureVerified = true;
    this.notifyReadyWaiters('加载失败，正在校验模型文件…');
    this.verifyFileIntegrity((status: string): void => {
      this.notifyReadyWaiters(status);
    }).then((valid: boolean): void => {
      this.finishPrewarm(false, valid
        ? '模型文件完整，但本地引擎加载失败'
        : '模型文件损坏，请重新下载');
    }).catch((): void => {
      this.finishPrewarm(false, '模型校验失败，请重新下载');
    });
  }

  private prewarmInWorker(): void {
    if (this._prewarming || this._state === LlmState.READY) return;
    this._prewarming = true;
    this.notifyReadyWaiters('正在加载本地模型…');
    try {
      this.llmWorker = new worker.ThreadWorker(
        'entry/ets/workers/LlmWorker.ets', { name: 'LLM Lazy Load' }
      );
      this.llmWorker.onmessage = (e: MessageEvents): void => {
        if (e.data['msgType'] !== 'llm-prewarm-result') return;
        const ok: boolean = e.data['success'] as boolean;
        this.llmWorker?.terminate();
        this.llmWorker = undefined;
        if (ok) {
          this.finishPrewarm(true, '本地模型已加载');
        } else {
          this.verifyAfterLoadFailure();
        }
      };
      this.llmWorker.postMessage({ msgType: 'prewarmLlm', modelDir: this._modelDir });
    } catch (_) {
      this.verifyAfterLoadFailure();
    }
  }

  private async verifyFileIntegrity(onStatus?: (status: string) => void): Promise<boolean> {
    if (this._verifying) {
      onStatus?.('模型正在校验中');
      return false;
    }
    const size: number = this.getFileSize(this.modelPath);
    const source: LlmModelSource | undefined = this.sourceForSize(size);
    if (!source) {
      this._integrityFailed = size > 0;
      this._state = size > 0 ? LlmState.ERROR : LlmState.UNAVAILABLE;
      onStatus?.(size > 0 ? '模型大小异常，请重新下载' : '本地模型未安装');
      return false;
    }

    this._verifying = true;
    this._state = LlmState.VERIFYING;
    onStatus?.('正在校验模型 SHA-256…');
    try {
      const actual: string = await this.downloader.calculateSha256(this.modelPath);
      const valid: boolean = actual === source.sha256.toLowerCase();
      this._integrityFailed = !valid;
      this.writeManifest(source, valid, actual);
      this._state = valid ? LlmState.INSTALLED : LlmState.ERROR;
      onStatus?.(valid ? '模型校验通过' : '模型已损坏，请重新下载');
      return valid;
    } catch (e) {
      this._state = LlmState.ERROR;
      onStatus?.('模型校验失败: ' + String(e));
      return false;
    } finally {
      this._verifying = false;
    }
  }

  async verifyInstalledModel(onStatus?: (status: string) => void): Promise<boolean> {
    if (this._state === LlmState.DOWNLOADING) {
      onStatus?.('模型正在下载，暂时无法校验');
      return false;
    }
    this.releaseRuntime(true);
    return this.verifyFileIntegrity(onStatus);
  }

  async downloadAndSetup(
    onProgress?: (pct: number, speed: number) => void,
    onStatus?: (s: string) => void
  ): Promise<boolean> {
    if (!this._modelDir) {
      onStatus?.('LlmManager 未初始化');
      return false;
    }
    if (this.checkModelSync()) return this.ensureReady(onStatus);

    this._integrityFailed = false;
    this._state = LlmState.DOWNLOADING;
    onStatus?.('正在下载 GGUF 模型');
    let selectedSource: LlmModelSource | undefined;
    const orderedSources: LlmModelSource[] = MODEL_SOURCES.slice();
    orderedSources.sort((a: LlmModelSource, b: LlmModelSource): number => {
      const aBytes: number = this.downloader.getPartialSize(this.modelPath, a.sha256);
      const bBytes: number = this.downloader.getPartialSize(this.modelPath, b.sha256);
      return bBytes - aBytes;
    });

    for (const source of orderedSources) {
      onStatus?.('正在连接 ' + source.name);
      const ok: boolean = await this.downloader.download({
        sourceName: source.name,
        url: source.url,
        destPath: this.modelPath,
        expectedSize: source.size,
        expectedSha256: source.sha256,
        onProgress,
        onStatus,
      });
      if (ok) {
        selectedSource = source;
        break;
      }
      if (this.downloader.getState() === DownloadState.IDLE) break;
    }

    if (!selectedSource) {
      this._state = LlmState.ERROR;
      onStatus?.('本地模型下载失败');
      return false;
    }
    this.writeManifest(selectedSource, true, selectedSource.sha256);
    this._state = LlmState.INSTALLED;
    this._loadFailureVerified = false;
    return this.ensureReady(onStatus);
  }

  cancelDownload(): void {
    this.downloader.cancel();
    this._state = this.checkModelSync() ? LlmState.INSTALLED : LlmState.UNAVAILABLE;
  }

  deleteInstalledModel(onStatus?: (status: string) => void): boolean {
    if (this._verifying) {
      onStatus?.('模型正在校验，请稍后再删除');
      return false;
    }
    this.downloader.cancel();
    this.releaseRuntime(true);
    const paths: string[] = [
      this.modelPath,
      this.manifestPath,
      this.manifestPath + '.tmp',
    ];
    for (let i: number = 0; i < paths.length; i++) {
      try { fileIo.unlinkSync(paths[i]); } catch (_) {}
    }
    this.downloader.removePartialDownloads(this.modelPath);
    this._integrityFailed = false;
    this._loadFailureVerified = false;
    this._state = LlmState.UNAVAILABLE;
    const removed: boolean = this.getFileSize(this.modelPath) === 0;
    onStatus?.(removed ? '本地模型已删除' : '本地模型删除失败');
    return removed;
  }

  /** Release mmap/context/KV allocations while retaining installation state. */
  releaseRuntime(force: boolean = false): void {
    if (!force && this._runtimeLeaseCount > 0) {
      console.info('OHOS_LLM: skip runtime release while diagnostic lease is active');
      return;
    }
    this.llmWorker?.terminate();
    this.llmWorker = undefined;
    this._prewarming = false;
    const waiters: LlmReadyWaiter[] = this._readyWaiters;
    this._readyWaiters = [];
    for (let i: number = 0; i < waiters.length; i++) {
      waiters[i].resolve(false);
    }
    try {
      nativeLib.releaseLocalLlm();
    } catch (e) {
      console.warn('OHOS_LLM: release runtime failed: ' + String(e));
    }
    this._state = this.checkModelSync() ? LlmState.INSTALLED :
      (this.getFileSize(this.modelPath) > 0 ? LlmState.ERROR : LlmState.UNAVAILABLE);
  }

  reset(): void {
    this.releaseRuntime();
  }
}
