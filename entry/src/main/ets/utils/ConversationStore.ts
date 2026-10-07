import { fileIo } from '@kit.CoreFileKit';
import { EncryptedStore } from './EncryptedStore';

export interface StoredHistorySession {
  id: string;
  title: string;
  time: string;
}

export interface StoredChatItem {
  msg: string;
  from: 'user' | 'assistant';
  source?: string;
}

export interface StoredConversation {
  session: StoredHistorySession;
  chatList: StoredChatItem[];
}

/** Encrypted conversation files with a single ordered I/O queue. */
export class ConversationStore {
  private static historyDir: string = '';
  private static writeQueue: Promise<void> = Promise.resolve();

  static init(filesDir: string): void {
    this.historyDir = filesDir + '/history';
    this.ensureDir();
  }

  static save(data: StoredConversation): Promise<void> {
    const path: string = this.pathFor(data.session.id);
    const operation: Promise<void> = this.writeQueue.then((): Promise<void> =>
      EncryptedStore.write(path, data)
    );
    this.writeQueue = operation.catch((_e: Error): void => {});
    return operation;
  }

  static async list(): Promise<StoredHistorySession[]> {
    await this.writeQueue;
    this.ensureDir();
    const result: StoredHistorySession[] = [];
    let files: string[] = [];
    try { files = fileIo.listFileSync(this.historyDir); } catch (_) { return result; }
    for (let i: number = 0; i < files.length; i++) {
      if (!files[i].endsWith('.json')) continue;
      const id: string = files[i].substring(0, files[i].length - 5);
      if (!this.validId(id)) continue;
      const data: StoredConversation | null = await EncryptedStore.read<StoredConversation>(this.pathFor(id));
      if (data && data.session && data.session.id) result.push(data.session);
    }
    console.info('OHOS_HISTORY: encrypted list loaded count=' + result.length);
    return result;
  }

  static async load(id: string): Promise<StoredConversation | null> {
    await this.writeQueue;
    return EncryptedStore.read<StoredConversation>(this.pathFor(id));
  }

  static async delete(id: string): Promise<void> {
    await this.writeQueue;
    try { fileIo.unlinkSync(this.pathFor(id)); } catch (_) {}
  }

  static async clear(): Promise<void> {
    await this.writeQueue;
    this.ensureDir();
    let files: string[] = [];
    try { files = fileIo.listFileSync(this.historyDir); } catch (_) { return; }
    for (let i: number = 0; i < files.length; i++) {
      if (!files[i].endsWith('.json')) continue;
      const id: string = files[i].substring(0, files[i].length - 5);
      if (!this.validId(id)) continue;
      try { fileIo.unlinkSync(this.pathFor(id)); } catch (_) {}
    }
  }

  private static pathFor(id: string): string {
    if (!this.historyDir) throw new Error('conversation store is not initialized');
    if (!this.validId(id)) throw new Error('invalid conversation id');
    return this.historyDir + '/' + id + '.json';
  }

  private static validId(id: string): boolean {
    return /^\d{10,20}$/.test(id);
  }

  private static ensureDir(): void {
    if (!this.historyDir) return;
    try {
      if (fileIo.accessSync(this.historyDir)) return;
    } catch (_) {}
    try { fileIo.mkdirSync(this.historyDir, true); } catch (_) {}
  }
}
