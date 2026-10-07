import { EncryptedStore } from './EncryptedStore';

export interface MemoRecord {
  id: number;
  content: string;
  createdAtMs: number;
}

export interface MemoResult {
  success: boolean;
  message: string;
}

interface MemoStoreData {
  version: number;
  memos: MemoRecord[];
}

export class MemoService {
  private static storePath: string = '';
  private static records: MemoRecord[] = [];
  private static ready: Promise<void> = Promise.resolve();

  static init(filesDir: string): Promise<void> {
    this.storePath = filesDir + '/memos/memos.json';
    this.ready = this.load();
    return this.ready;
  }

  static async add(content: string): Promise<MemoResult> {
    await this.ready;
    let id: number = Date.now();
    while (this.records.some((item: MemoRecord): boolean => item.id === id)) id++;
    const record: MemoRecord = { id, content, createdAtMs: Date.now() };
    this.records.push(record);
    try {
      await this.persist();
      return { success: true, message: '已经记下：' + content + '。' };
    } catch (e) {
      this.records = this.records.filter((item: MemoRecord): boolean => item.id !== record.id);
      console.error('OHOS_MEMO: encrypted add failed=' + String(e));
      return { success: false, message: '备忘加密保存失败，请稍后再试。' };
    }
  }

  static getRecords(): MemoRecord[] {
    return this.records.slice().sort(
      (a: MemoRecord, b: MemoRecord): number => b.createdAtMs - a.createdAtMs
    );
  }

  static async describeList(): Promise<MemoResult> {
    await this.ready;
    const sorted: MemoRecord[] = this.getRecords();
    if (sorted.length === 0) return { success: true, message: '当前没有备忘。' };
    const parts: string[] = [];
    const count: number = Math.min(5, sorted.length);
    for (let i: number = 0; i < count; i++) parts.push((i + 1) + '，' + sorted[i].content);
    let suffix: string = '。';
    if (sorted.length > count) suffix = '。另外还有' + (sorted.length - count) + '条，可以在备忘列表中查看。';
    return { success: true, message: '你有' + sorted.length + '条备忘：' + parts.join('；') + suffix };
  }

  static async deleteLast(): Promise<MemoResult> {
    await this.ready;
    const sorted: MemoRecord[] = this.getRecords();
    if (sorted.length === 0) return { success: true, message: '当前没有可以删除的备忘。' };
    return this.deleteById(sorted[0].id);
  }

  static async deleteById(id: number): Promise<MemoResult> {
    await this.ready;
    let target: MemoRecord | undefined;
    for (let i: number = 0; i < this.records.length; i++) {
      if (this.records[i].id === id) {
        target = this.records[i];
        break;
      }
    }
    if (!target) return { success: false, message: '这条备忘已经不存在了。' };
    const previous: MemoRecord[] = this.records.slice();
    this.records = this.records.filter((item: MemoRecord): boolean => item.id !== id);
    try {
      await this.persist();
      return { success: true, message: '已删除备忘“' + target.content + '”。' };
    } catch (e) {
      this.records = previous;
      console.error('OHOS_MEMO: encrypted delete failed=' + String(e));
      return { success: false, message: '删除备忘失败，请稍后再试。' };
    }
  }

  static async clear(): Promise<MemoResult> {
    await this.ready;
    const previous: MemoRecord[] = this.records.slice();
    this.records = [];
    try {
      await this.persist();
      return { success: true, message: '已清空全部备忘。' };
    } catch (e) {
      this.records = previous;
      console.error('OHOS_MEMO: encrypted clear failed=' + String(e));
      return { success: false, message: '清空备忘失败，请稍后再试。' };
    }
  }

  private static async load(): Promise<void> {
    const data: MemoStoreData | null = await EncryptedStore.read<MemoStoreData>(this.storePath);
    this.records = data && data.memos ? data.memos : [];
    console.info('OHOS_MEMO: encrypted store loaded count=' + this.records.length);
  }

  private static async persist(): Promise<void> {
    const data: MemoStoreData = { version: 2, memos: this.records };
    await EncryptedStore.write(this.storePath, data);
  }
}
