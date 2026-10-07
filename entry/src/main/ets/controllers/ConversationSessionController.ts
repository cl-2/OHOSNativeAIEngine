import {
  ConversationStore,
  StoredChatItem,
  StoredConversation,
  StoredHistorySession
} from '../utils/ConversationStore';

export interface ConversationChatItem {
  msg: string;
  from: 'user' | 'assistant';
  source?: string;
}

export interface ConversationSessionItem {
  id: string;
  title: string;
  time: string;
}

export interface ConversationRestoreResult {
  token: number;
  sessionId: string;
  items: ConversationChatItem[];
}

/** Owns encrypted conversation persistence and stale-restore protection. */
export class ConversationSessionController {
  private currentId: string = '';
  private restoreGeneration: number = 0;

  init(filesDir: string): void { ConversationStore.init(filesDir); }
  get currentConversationId(): string { return this.currentId; }

  ensureCurrentConversationId(): string {
    if (!this.currentId) this.currentId = Date.now().toString();
    return this.currentId;
  }

  isCurrent(id: string): boolean { return this.currentId === id; }

  startNewConversation(): void {
    this.restoreGeneration++;
    this.currentId = '';
  }

  invalidateRestore(): void { this.restoreGeneration++; }

  async save(items: ConversationChatItem[]): Promise<void> {
    if (items.length < 2) return;
    const now: Date = new Date();
    this.ensureCurrentConversationId();
    const time: string = (now.getMonth() + 1) + '/' + now.getDate() + ' ' +
      now.getHours().toString().padStart(2, '0') + ':' + now.getMinutes().toString().padStart(2, '0');
    let title: string = '新对话';
    for (let i: number = 0; i < items.length; i++) {
      if (items[i].from !== 'user') continue;
      const text: string = items[i].msg;
      title = text.length > 20 ? text.substring(0, 20) + '…' : text;
      break;
    }
    const storedItems: StoredChatItem[] = items.map((item: ConversationChatItem): StoredChatItem => ({
      msg: item.msg,
      from: item.from,
      source: item.source
    }));
    await ConversationStore.save({
      session: { id: this.currentId, title, time },
      chatList: storedItems
    });
  }

  async list(): Promise<ConversationSessionItem[]> {
    const stored: StoredHistorySession[] = await ConversationStore.list();
    const sessions: ConversationSessionItem[] = stored.map(
      (item: StoredHistorySession): ConversationSessionItem => ({
        id: item.id,
        title: item.title,
        time: item.time
      })
    );
    sessions.sort((a: ConversationSessionItem, b: ConversationSessionItem): number =>
      parseInt(b.id) - parseInt(a.id));
    return sessions;
  }

  async restore(sessionId: string): Promise<ConversationRestoreResult | null> {
    const token: number = ++this.restoreGeneration;
    this.currentId = sessionId;
    const data: StoredConversation | null = await ConversationStore.load(sessionId);
    if (!this.isRestoreCurrent(token, sessionId)) return null;
    const stored: StoredChatItem[] = data ? data.chatList : [];
    const items: ConversationChatItem[] = stored.map((item: StoredChatItem): ConversationChatItem => ({
      msg: item.msg,
      from: item.from,
      source: item.source || ''
    }));
    return { token, sessionId, items };
  }

  isRestoreCurrent(token: number, sessionId: string): boolean {
    return token === this.restoreGeneration && this.currentId === sessionId;
  }

  async delete(sessionId: string): Promise<boolean> {
    await ConversationStore.delete(sessionId);
    if (this.currentId !== sessionId) return false;
    this.restoreGeneration++;
    this.currentId = '';
    return true;
  }

  async clear(): Promise<void> {
    this.restoreGeneration++;
    await ConversationStore.clear();
    this.currentId = '';
  }
}
