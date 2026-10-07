/**
 * ModelDownloader - large model downloader
 *
 * Downloads to a source-specific .part file, supports HTTP Range resume,
 * verifies the exact size and SHA-256, then atomically renames the file.
 */

import { http } from '@kit.NetworkKit';
import { fileIo, hash, statfs } from '@kit.CoreFileKit';

export interface DownloadTask {
  sourceName?: string;
  url: string;
  destPath: string;
  expectedSize?: number;
  expectedSha256?: string;
  onProgress?: (percent: number, speedKBps: number) => void;
  onStatus?: (status: string) => void;
}

export enum DownloadState {
  IDLE = 'IDLE',
  DOWNLOADING = 'DOWNLOADING',
  VERIFYING = 'VERIFYING',
  COMPLETED = 'COMPLETED',
  FAILED = 'FAILED',
}

enum AttemptResult {
  SUCCESS = 'SUCCESS',
  FAILED = 'FAILED',
  RETRY_FRESH = 'RETRY_FRESH',
  CANCELLED = 'CANCELLED',
}

export class ModelDownloader {
  private state: DownloadState = DownloadState.IDLE;
  private bytesDownloaded: number = 0;
  private startTime: number = 0;
  private attemptStartBytes: number = 0;
  private abortController: boolean = false;
  private activeRequest?: http.HttpRequest;

  getState(): DownloadState { return this.state; }

  getPartialSize(destPath: string, expectedSha256: string): number {
    return this.getFileSize(this.getPartPath(destPath, expectedSha256));
  }

  async calculateSha256(path: string): Promise<string> {
    return (await hash.hash(path, 'sha256')).toLowerCase();
  }

  removePartialDownloads(destPath: string): void {
    this.cleanupSiblingParts(destPath);
  }

  cancel(): void {
    this.abortController = true;
    this.state = DownloadState.IDLE;
    try { this.activeRequest?.destroy(); } catch (_e) {}
    this.activeRequest = undefined;
  }

  async download(task: DownloadTask): Promise<boolean> {
    this.state = DownloadState.DOWNLOADING;
    this.abortController = false;

    const destDir: string = task.destPath.substring(0, task.destPath.lastIndexOf('/'));
    try { fileIo.mkdirSync(destDir, true); } catch (_e) {}

    const partPath: string = this.getPartPath(task.destPath, task.expectedSha256);
    let partSize: number = this.getFileSize(partPath);

    if (task.expectedSize && partSize > task.expectedSize) {
      this.removeFile(partPath);
      partSize = 0;
    }

    if (task.expectedSize && partSize === task.expectedSize) {
      task.onStatus?.('正在校验已下载的模型…');
      if (await this.verifyAndInstall(task, partPath)) return true;
      this.removeFile(partPath);
      partSize = 0;
    }

    if (!this.hasEnoughSpace(destDir, task.expectedSize, partSize)) {
      task.onStatus?.('存储空间不足，至少需要约 750MB 可用空间');
      this.state = DownloadState.FAILED;
      return false;
    }

    let result: AttemptResult = await this.downloadAttempt(task, partPath, partSize);
    if (result === AttemptResult.RETRY_FRESH && !this.abortController) {
      this.removeFile(partPath);
      result = await this.downloadAttempt(task, partPath, 0);
    }
    if (result !== AttemptResult.SUCCESS) {
      this.state = result === AttemptResult.CANCELLED ? DownloadState.IDLE : DownloadState.FAILED;
      return false;
    }

    return this.verifyAndInstall(task, partPath);
  }

  private async downloadAttempt(
    task: DownloadTask,
    partPath: string,
    resumeOffset: number
  ): Promise<AttemptResult> {
    let file: fileIo.File;
    try {
      const mode: number = resumeOffset > 0
        ? fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.APPEND
        : fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY | fileIo.OpenMode.TRUNC;
      file = fileIo.openSync(partPath, mode);
    } catch (e) {
      console.error('OHOS_DL: cannot open part file: ' + String(e));
      return AttemptResult.FAILED;
    }

    this.bytesDownloaded = resumeOffset;
    this.attemptStartBytes = resumeOffset;
    this.startTime = Date.now();
    let writeFailed: boolean = false;
    let firstDataReceived: boolean = false;
    const req: http.HttpRequest = http.createHttp();
    this.activeRequest = req;
    const sourceLabel: string = task.sourceName || '模型源';
    console.info('OHOS_DL: request start source=' + sourceLabel +
      ' resumeBytes=' + resumeOffset);

    req.on('dataReceive', (data: ArrayBuffer): void => {
      if (this.abortController || writeFailed || !data || data.byteLength === 0) return;
      if (!firstDataReceived) {
        firstDataReceived = true;
        const firstByteMs: number = Date.now() - this.startTime;
        console.info('OHOS_DL: first data source=' + sourceLabel + ' afterMs=' + firstByteMs);
        task.onStatus?.(sourceLabel + ' 已连接，开始接收数据');
      }
      try {
        const written: number = fileIo.writeSync(file.fd, data);
        if (written !== data.byteLength) {
          writeFailed = true;
          console.error('OHOS_DL: short write ' + written + '/' + data.byteLength);
          return;
        }
        this.bytesDownloaded += written;
        const elapsed: number = Math.max(0.001, (Date.now() - this.startTime) / 1000);
        const attemptBytes: number = this.bytesDownloaded - this.attemptStartBytes;
        const speed: number = attemptBytes / 1024 / elapsed;
        const pct: number = task.expectedSize && task.expectedSize > 0
          ? Math.min(99, Math.floor(this.bytesDownloaded * 100 / task.expectedSize))
          : 0;
        task.onProgress?.(pct, speed);
      } catch (e) {
        writeFailed = true;
        console.error('OHOS_DL: write error: ' + String(e));
      }
    });

    const headers: Record<string, string> = {};
    if (resumeOffset > 0) {
      headers['Range'] = 'bytes=' + resumeOffset + '-';
      task.onStatus?.('正在从 ' + this.formatMb(resumeOffset) + 'MB 继续下载…');
    }

    try {
      const responseCode: number = await req.requestInStream(task.url, {
        method: http.RequestMethod.GET,
        header: headers,
        connectTimeout: 10000,
        readTimeout: 60000,
      });
      fileIo.closeSync(file);
      req.destroy();
      this.activeRequest = undefined;

      if (this.abortController) return AttemptResult.CANCELLED;
      if (writeFailed) return AttemptResult.FAILED;
      if (resumeOffset > 0 && responseCode === 200) {
        console.warn('OHOS_DL: server ignored Range; restarting from zero');
        return AttemptResult.RETRY_FRESH;
      }
      if (responseCode !== 200 && responseCode !== 206) {
        console.error('OHOS_DL: HTTP response code=' + responseCode);
        this.removeFile(partPath);
        return AttemptResult.FAILED;
      }
      if (task.expectedSize && this.bytesDownloaded !== task.expectedSize) {
        console.error('OHOS_DL: incomplete file bytes=' + this.bytesDownloaded +
          ' expected=' + task.expectedSize);
        return AttemptResult.FAILED;
      }
      console.info('OHOS_DL: response complete source=' + sourceLabel +
        ' code=' + responseCode + ' elapsedMs=' + (Date.now() - this.startTime));
      return AttemptResult.SUCCESS;
    } catch (e) {
      try { fileIo.closeSync(file); } catch (_closeError) {}
      try { req.destroy(); } catch (_destroyError) {}
      this.activeRequest = undefined;
      if (this.abortController) return AttemptResult.CANCELLED;
      console.error('OHOS_DL: request interrupted source=' + sourceLabel +
        ' elapsedMs=' + (Date.now() - this.startTime) +
        ' part file preserved: ' + String(e) + ' ' + JSON.stringify(e));
      return AttemptResult.FAILED;
    }
  }

  private async verifyAndInstall(task: DownloadTask, partPath: string): Promise<boolean> {
    if (this.abortController) {
      this.state = DownloadState.IDLE;
      return false;
    }
    this.state = DownloadState.VERIFYING;
    task.onStatus?.('下载完成，正在校验 SHA-256…');
    const verifyStart: number = Date.now();

    if (task.expectedSize && this.getFileSize(partPath) !== task.expectedSize) {
      console.error('OHOS_DL: size verification failed');
      this.state = DownloadState.FAILED;
      return false;
    }

    if (task.expectedSha256) {
      try {
        const actual: string = await this.calculateSha256(partPath);
        if (actual !== task.expectedSha256.toLowerCase()) {
          console.error('OHOS_DL: SHA-256 mismatch actual=' + actual);
          this.removeFile(partPath);
          this.state = DownloadState.FAILED;
          return false;
        }
        console.info('OHOS_DL: SHA-256 verified in ' + (Date.now() - verifyStart) + 'ms');
        if (this.abortController) {
          this.state = DownloadState.IDLE;
          return false;
        }
      } catch (e) {
        console.error('OHOS_DL: SHA-256 failed: ' + String(e));
        this.state = DownloadState.FAILED;
        return false;
      }
    }

    if (this.abortController) {
      this.state = DownloadState.IDLE;
      return false;
    }
    try {
      this.removeFile(task.destPath);
      fileIo.renameSync(partPath, task.destPath);
      this.cleanupSiblingParts(task.destPath);
    } catch (e) {
      console.error('OHOS_DL: install model failed: ' + String(e));
      this.state = DownloadState.FAILED;
      return false;
    }

    this.state = DownloadState.COMPLETED;
    task.onProgress?.(100, 0);
    task.onStatus?.('模型校验完成');
    return true;
  }

  private hasEnoughSpace(destDir: string, expectedSize?: number, existingSize: number = 0): boolean {
    if (!expectedSize) return true;
    try {
      const remaining: number = Math.max(0, expectedSize - existingSize);
      const reserveBytes: number = 256 * 1024 * 1024;
      return statfs.getFreeSizeSync(destDir) >= remaining + reserveBytes;
    } catch (e) {
      console.warn('OHOS_DL: free-space check unavailable: ' + String(e));
      return true;
    }
  }

  private getFileSize(path: string): number {
    try { return fileIo.statSync(path).size; } catch (_e) { return 0; }
  }

  private getPartPath(destPath: string, expectedSha256?: string): string {
    const resumeKey: string = expectedSha256
      ? expectedSha256.toLowerCase().substring(0, 12)
      : 'download';
    return destPath + '.' + resumeKey + '.part';
  }

  private removeFile(path: string): void {
    try { fileIo.unlinkSync(path); } catch (_e) {}
  }

  private cleanupSiblingParts(destPath: string): void {
    const slash: number = destPath.lastIndexOf('/');
    const dir: string = destPath.substring(0, slash);
    const name: string = destPath.substring(slash + 1) + '.';
    try {
      const files: string[] = fileIo.listFileSync(dir);
      for (let i: number = 0; i < files.length; i++) {
        if (files[i].startsWith(name) && files[i].endsWith('.part')) {
          this.removeFile(dir + '/' + files[i]);
        }
      }
    } catch (_e) {}
  }

  private formatMb(bytes: number): string {
    return (bytes / 1024 / 1024).toFixed(1);
  }
}
