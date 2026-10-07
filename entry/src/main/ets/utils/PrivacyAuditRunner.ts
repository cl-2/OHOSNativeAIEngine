/**
 * Read-only privacy storage audit.
 *
 * The audit only inspects directory entries and the fixed-size envelope header.
 * It never decrypts, parses, logs, or returns user content and file names.
 */
import { fileIo } from '@kit.CoreFileKit';

export type PrivacyAuditStatus = 'pass' | 'warning' | 'failure';

export interface PrivacyAuditItem {
  id: string;
  label: string;
  status: PrivacyAuditStatus;
  detail: string;
  fileCount: number;
}

export interface PrivacyAuditReport {
  checkedAtMs: number;
  encryptedFileCount: number;
  warningCount: number;
  failureCount: number;
  items: PrivacyAuditItem[];
}

interface FileGroupResult {
  encryptedCount: number;
  invalidCount: number;
  tempCount: number;
  readErrorCount: number;
}

const V2_HEADER_BYTES: number[] = [79, 72, 79, 83, 69, 78, 67, 50, 58]; // OHOSENC2:

export function isAuthenticatedEnvelopeHeader(bytes: Uint8Array, length: number): boolean {
  if (length < V2_HEADER_BYTES.length) return false;
  for (let i: number = 0; i < V2_HEADER_BYTES.length; i++) {
    if (bytes[i] !== V2_HEADER_BYTES[i]) return false;
  }
  return true;
}

export class PrivacyAuditRunner {
  static run(filesDir: string): PrivacyAuditReport {
    const items: PrivacyAuditItem[] = [];
    let encryptedFileCount: number = 0;

    const fixedStores: string[] = [
      filesDir + '/llm/remote_llm_config.enc',
      filesDir + '/llm/llm_routing_mode.enc',
      filesDir + '/memory/profile.json.enc',
      filesDir + '/memos/memos.json',
      filesDir + '/reminders/reminders.json'
    ];
    const fixedResult: FileGroupResult = this.inspectKnownFiles(fixedStores);
    encryptedFileCount += fixedResult.encryptedCount;
    items.push(this.toAuditItem('personal_stores', '个人数据与连接配置', fixedResult));

    const historyResult: FileGroupResult = this.inspectDirectory(filesDir + '/history', '.json');
    encryptedFileCount += historyResult.encryptedCount;
    items.push(this.toAuditItem('conversation_history', '历史会话', historyResult));

    const legacyMemoryPath: string = filesDir + '/memory/profile.json';
    if (this.exists(legacyMemoryPath)) {
      items.push({
        id: 'legacy_plaintext',
        label: '旧版明文残留',
        status: 'failure',
        detail: '发现旧版记忆明文文件；加密迁移未完整收口。',
        fileCount: 1
      });
    } else {
      items.push({
        id: 'legacy_plaintext',
        label: '旧版明文残留',
        status: 'pass',
        detail: '未发现已知旧版明文记忆文件。',
        fileCount: 0
      });
    }

    const ragPlainCount: number = this.countFiles(filesDir + '/documents', '.txt') +
      this.countExistingFiles([
        filesDir + '/index/inverted.json',
        filesDir + '/index/meta.json'
      ]);
    if (ragPlainCount > 0) {
      items.push({
        id: 'rag_boundary',
        label: '本地文档边界',
        status: 'warning',
        detail: '发现本地文档或检索索引；当前轻量 RAG 仍以明文保存在应用沙箱。',
        fileCount: ragPlainCount
      });
    } else {
      items.push({
        id: 'rag_boundary',
        label: '本地文档边界',
        status: 'pass',
        detail: '当前未保存本地 RAG 文档。',
        fileCount: 0
      });
    }

    let warningCount: number = 0;
    let failureCount: number = 0;
    for (let i: number = 0; i < items.length; i++) {
      if (items[i].status === 'warning') warningCount++;
      if (items[i].status === 'failure') failureCount++;
    }
    return {
      checkedAtMs: Date.now(),
      encryptedFileCount,
      warningCount,
      failureCount,
      items
    };
  }

  private static inspectKnownFiles(paths: string[]): FileGroupResult {
    const result: FileGroupResult = this.emptyResult();
    for (let i: number = 0; i < paths.length; i++) {
      if (this.exists(paths[i])) this.inspectEnvelope(paths[i], result);
      if (this.exists(paths[i] + '.tmp')) result.tempCount++;
    }
    return result;
  }

  private static inspectDirectory(dir: string, suffix: string): FileGroupResult {
    const result: FileGroupResult = this.emptyResult();
    let files: string[] = [];
    try { files = fileIo.listFileSync(dir); } catch (_) { return result; }
    for (let i: number = 0; i < files.length; i++) {
      if (files[i].endsWith(suffix + '.tmp')) {
        result.tempCount++;
      } else if (files[i].endsWith(suffix)) {
        this.inspectEnvelope(dir + '/' + files[i], result);
      }
    }
    return result;
  }

  private static inspectEnvelope(path: string, result: FileGroupResult): void {
    let file: fileIo.File | null = null;
    try {
      file = fileIo.openSync(path, fileIo.OpenMode.READ_ONLY);
      const buffer: ArrayBuffer = new ArrayBuffer(V2_HEADER_BYTES.length);
      const length: number = fileIo.readSync(file.fd, buffer, { offset: 0, length: V2_HEADER_BYTES.length });
      if (isAuthenticatedEnvelopeHeader(new Uint8Array(buffer), length)) result.encryptedCount++;
      else result.invalidCount++;
    } catch (_) {
      result.readErrorCount++;
    } finally {
      if (file !== null) {
        try { fileIo.closeSync(file); } catch (_) {}
      }
    }
  }

  private static toAuditItem(id: string, label: string, result: FileGroupResult): PrivacyAuditItem {
    const count: number = result.encryptedCount + result.invalidCount + result.readErrorCount;
    if (result.invalidCount > 0 || result.readErrorCount > 0) {
      return {
        id,
        label,
        status: 'failure',
        detail: '存在 ' + result.invalidCount + ' 个非认证密文文件，' + result.readErrorCount + ' 个文件头读取失败。',
        fileCount: count
      };
    }
    if (result.tempCount > 0) {
      return {
        id,
        label,
        status: 'warning',
        detail: '认证密文正常，但发现 ' + result.tempCount + ' 个未完成写入的临时文件。',
        fileCount: count
      };
    }
    if (count === 0) {
      return { id, label, status: 'pass', detail: '尚无对应数据文件。', fileCount: 0 };
    }
    return {
      id,
      label,
      status: 'pass',
      detail: count + ' 个文件均为 HUKS 认证密文。',
      fileCount: count
    };
  }

  private static countFiles(dir: string, suffix: string): number {
    let files: string[] = [];
    try { files = fileIo.listFileSync(dir); } catch (_) { return 0; }
    let count: number = 0;
    for (let i: number = 0; i < files.length; i++) {
      if (files[i].endsWith(suffix)) count++;
    }
    return count;
  }

  private static countExistingFiles(paths: string[]): number {
    let count: number = 0;
    for (let i: number = 0; i < paths.length; i++) {
      if (this.exists(paths[i])) count++;
    }
    return count;
  }

  private static exists(path: string): boolean {
    try { return fileIo.accessSync(path); } catch (_) { return false; }
  }

  private static emptyResult(): FileGroupResult {
    return { encryptedCount: 0, invalidCount: 0, tempCount: 0, readErrorCount: 0 };
  }
}
