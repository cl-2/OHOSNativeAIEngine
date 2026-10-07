/**
 * MemoryManager.ts - 混合记忆管理器
 *
 * 管理两层记忆：
 * 1. 短期记忆（session）：当前对话上下文，超时自动清理
 * 2. 长期记忆（profile）：用户画像 + 已知事实，持久化到 JSON 文件
 */

import { fileIo } from '@kit.CoreFileKit';
import { EncryptedStore } from './EncryptedStore';
import {
  extractStructuredMemory,
  MemoryFieldUpdate,
  StructuredMemoryExtraction
} from './MemoryExtractor';

// ===== 数据结构 =====

interface UserProfile {
  /** 用户偏好 */
  preferences: Record<string, string>;
  /** 已知事实列表（如"用户姓张、家里有猫"） */
  facts: string[];
  /** 常聊话题 */
  topics: string[];
  /** 最后更新时间 */
  updatedAt: string;
  /** 对话次数 */
  conversationCount: number;
}

interface SessionState {
  conversationId: string;
  startTime: number;
  lastActivityTime: number;
  turnCount: number;
  /** 当前对话的意图摘要 */
  intentSummary: string;
}

export interface MemoryPreferenceItem {
  key: string;
  value: string;
}

export interface UserMemorySnapshot {
  preferences: MemoryPreferenceItem[];
  facts: string[];
  topics: string[];
  updatedAt: string;
  conversationCount: number;
}

type MemoryMutationKind = 'add_fact' | 'set_preference' | 'add_topic' | 'increment' | 'reset';

interface PendingMemoryMutation {
  kind: MemoryMutationKind;
  key: string;
  value: string;
}

// ===== 常量 =====

const SESSION_TIMEOUT_MS: number = 30 * 60 * 1000; // 30 分钟无活动自动过期
const MAX_FACTS: number = 50; // 最多保留 50 条事实
const MAX_TOPICS: number = 20;

// ===== 默认 Profile =====

function defaultProfile(): UserProfile {
  return {
    preferences: {},
    facts: [],
    topics: [],
    updatedAt: new Date().toISOString(),
    conversationCount: 0,
  };
}

// ============================================================
// MemoryManager
// ============================================================

export class MemoryManager {
  private static _profile: UserProfile | null = null;
  private static _storagePath: string = '';
  private static _initialized: boolean = false;
  private static _profileReady: boolean = false;
  private static _readyPromise: Promise<void> = Promise.resolve();
  private static _pendingMutations: PendingMemoryMutation[] = [];
  private static _saveQueue: Promise<void> = Promise.resolve();
  private static _sessions: Map<string, SessionState> = new Map();
  private static _cleanupTimer: number = -1;

  /**
   * 初始化（由 Index.ets 在 aboutToAppear 中调用）
   */
  static init(filesDir: string): Promise<void> {
    if (this._initialized) return this._readyPromise;
    this._storagePath = filesDir + '/memory';
    try { fileIo.mkdirSync(this._storagePath, true); } catch (_e) {}
    this._profile = defaultProfile();
    this._startCleanupTimer();
    this._initialized = true;
    this._readyPromise = this._initializeProfile();
    return this._readyPromise;
  }

  // ======================== Session 管理 ========================

  /**
   * 创建/更新 session
   */
  static touchSession(conversationId: string): void {
    const now: number = Date.now();
    const existing: SessionState | undefined = this._sessions.get(conversationId);
    if (existing) {
      existing.lastActivityTime = now;
      existing.turnCount++;
    } else {
      this._sessions.set(conversationId, {
        conversationId,
        startTime: now,
        lastActivityTime: now,
        turnCount: 1,
        intentSummary: '',
      });
    }
  }

  /**
   * 更新 session 的意图摘要
   */
  static updateIntentSummary(conversationId: string, summary: string): void {
    const session: SessionState | undefined = this._sessions.get(conversationId);
    if (session) {
      session.intentSummary = summary;
    }
  }

  /**
   * 获取 session 信息
   */
  static getSession(conversationId: string): SessionState | undefined {
    return this._sessions.get(conversationId);
  }

  /**
   * 清理过期 session
   */
  static cleanupExpiredSessions(): number {
    const now: number = Date.now();
    let removed: number = 0;
    this._sessions.forEach((session, id) => {
      if (now - session.lastActivityTime > SESSION_TIMEOUT_MS) {
        this._sessions.delete(id);
        removed++;
      }
    });
    if (removed > 0) {
      console.info('OHOS_Memory: cleaned ' + removed + ' expired sessions');
    }
    return removed;
  }

  // ======================== 长期记忆（Profile） ========================

  /**
   * 获取完整 profile
   */
  static getProfile(): UserProfile {
    if (!this._profile) this._profile = defaultProfile();
    return this._profile;
  }

  /**
   * 获取 System Prompt 中要注入的记忆文本
   */
  static getMemoryPrompt(): string {
    if (!this._profileReady) return '';
    const p: UserProfile = this.getProfile();
    const parts: string[] = [];

    // 偏好
    const prefs: string[] = Object.keys(p.preferences);
    if (prefs.length > 0) {
      parts.push('关于用户的偏好：');
      for (let i: number = 0; i < prefs.length; i++) {
        parts.push('- ' + prefs[i] + '：' + p.preferences[prefs[i]]);
      }
    }

    // 事实
    if (p.facts.length > 0) {
      parts.push('关于用户，我知道以下事实：');
      // 只取最近 20 条
      const recent: string[] = p.facts.slice(-20);
      for (let i: number = 0; i < recent.length; i++) {
        parts.push('- ' + recent[i]);
      }
    }

    // 话题
    if (p.topics.length > 0) {
      parts.push('用户常聊话题：' + p.topics.slice(-5).join('、'));
    }

    parts.push('对话次数：' + p.conversationCount + ' 次');

    return parts.length > 0 ? parts.join('\n') : '';
  }

  /**
   * 添加一条事实
   */
  static addFact(fact: string): void {
    this._mutate({ kind: 'add_fact', key: '', value: fact });
  }

  /**
   * 设置偏好
   */
  static setPreference(key: string, value: string): void {
    this._mutate({ kind: 'set_preference', key, value });
  }

  /**
   * 添加话题
   */
  static addTopic(topic: string): void {
    this._mutate({ kind: 'add_topic', key: '', value: topic });
  }

  /**
   * 增加对话计数
   */
  static incrementConversationCount(): void {
    this._mutate({ kind: 'increment', key: '', value: '' });
  }

  /**
   * 重置所有记忆
   */
  static resetAll(): void {
    this._sessions.clear();
    this._mutate({ kind: 'reset', key: '', value: '' });
  }

  /** Returns a detached snapshot suitable for UI display. */
  static async getUserMemorySnapshot(): Promise<UserMemorySnapshot> {
    await this._readyPromise;
    const p: UserProfile = this.getProfile();
    const preferences: MemoryPreferenceItem[] = [];
    const keys: string[] = Object.keys(p.preferences);
    for (let i: number = 0; i < keys.length; i++) {
      preferences.push({ key: keys[i], value: p.preferences[keys[i]] });
    }
    return {
      preferences,
      facts: p.facts.slice(),
      topics: p.topics.slice(),
      updatedAt: p.updatedAt,
      conversationCount: p.conversationCount
    };
  }

  static async deleteFact(fact: string): Promise<boolean> {
    await this._readyPromise;
    const p: UserProfile = this.getProfile();
    const index: number = p.facts.indexOf(fact);
    if (index < 0) return false;
    p.facts.splice(index, 1);
    p.updatedAt = new Date().toISOString();
    await this._saveProfile();
    console.info('OHOS_Memory: fact deleted, count=' + p.facts.length);
    return true;
  }

  static async deletePreference(key: string): Promise<boolean> {
    await this._readyPromise;
    const p: UserProfile = this.getProfile();
    if (!Object.prototype.hasOwnProperty.call(p.preferences, key)) return false;
    delete p.preferences[key];
    p.updatedAt = new Date().toISOString();
    await this._saveProfile();
    console.info('OHOS_Memory: preference deleted, count=' + Object.keys(p.preferences).length);
    return true;
  }

  static async deleteTopic(topic: string): Promise<boolean> {
    await this._readyPromise;
    const p: UserProfile = this.getProfile();
    const index: number = p.topics.indexOf(topic);
    if (index < 0) return false;
    p.topics.splice(index, 1);
    p.updatedAt = new Date().toISOString();
    await this._saveProfile();
    console.info('OHOS_Memory: topic deleted, count=' + p.topics.length);
    return true;
  }

  static async clearUserMemory(): Promise<void> {
    await this._readyPromise;
    this._sessions.clear();
    this._profile = defaultProfile();
    await this._saveProfile();
    console.info('OHOS_Memory: user memory cleared');
  }

  // ======================== 内部方法 ========================

  private static _plainProfilePath(): string {
    return this._storagePath + '/profile.json';
  }

  private static _encryptedProfilePath(): string {
    return this._storagePath + '/profile.json.enc';
  }

  private static async _initializeProfile(): Promise<void> {
    let encrypted: UserProfile | null = null;
    let plain: UserProfile | null = null;
    try {
      const stored: UserProfile | null = await EncryptedStore.read<UserProfile>(this._encryptedProfilePath());
      if (stored) encrypted = this._normalizeProfile(stored);
    } catch (e) {
      console.error('OHOS_Memory: encrypted profile read failed=' + String(e));
    }
    try {
      const content: string = fileIo.readTextSync(this._plainProfilePath());
      plain = this._normalizeProfile(JSON.parse(content) as UserProfile);
    } catch (_) {}

    if (encrypted && plain) {
      const encryptedTime: number = Date.parse(encrypted.updatedAt) || 0;
      const plainTime: number = Date.parse(plain.updatedAt) || 0;
      this._profile = plainTime > encryptedTime ? plain : encrypted;
    } else {
      this._profile = encrypted || plain || defaultProfile();
    }

    this._profileReady = true;
    if (this._pendingMutations.length > 0) {
      const pending: PendingMemoryMutation[] = this._pendingMutations.slice();
      this._pendingMutations = [];
      for (let i: number = 0; i < pending.length; i++) this._applyMutation(pending[i]);
    }

    try {
      await this._writeProfileSnapshot();
      try { fileIo.unlinkSync(this._plainProfilePath()); } catch (_) {}
      console.info('OHOS_Memory: encrypted profile ready facts=' + this.getProfile().facts.length +
        ' topics=' + this.getProfile().topics.length + ' conversations=' + this.getProfile().conversationCount);
    } catch (e) {
      console.error('OHOS_Memory: encrypted profile migration failed, plaintext kept=' + String(e));
    }
  }

  private static _saveProfile(): Promise<void> {
    if (!this._profile || !this._profileReady) return Promise.resolve();
    const snapshot: UserProfile = this._cloneProfile(this._profile);
    const operation: Promise<void> = this._saveQueue.then((): Promise<void> =>
      EncryptedStore.write(this._encryptedProfilePath(), snapshot)
    );
    this._saveQueue = operation.catch((e: Error): void => {
      console.error('OHOS_Memory: encrypted profile save failed=' + String(e));
    });
    return operation;
  }

  private static async _writeProfileSnapshot(): Promise<void> {
    if (!this._profile) return;
    await this._saveQueue;
    await EncryptedStore.write(this._encryptedProfilePath(), this._cloneProfile(this._profile));
  }

  private static _mutate(mutation: PendingMemoryMutation): void {
    if (!this._profileReady) {
      this._pendingMutations.push(mutation);
      return;
    }
    if (this._applyMutation(mutation)) this._saveProfile();
  }

  private static _applyMutation(mutation: PendingMemoryMutation): boolean {
    if (mutation.kind === 'reset') {
      this._profile = defaultProfile();
      console.info('OHOS_Memory: reset all memories');
      return true;
    }
    const p: UserProfile = this.getProfile();
    if (mutation.kind === 'add_fact') {
      for (let i: number = 0; i < p.facts.length; i++) if (p.facts[i] === mutation.value) return false;
      p.facts.push(mutation.value);
      if (p.facts.length > MAX_FACTS) p.facts = p.facts.slice(-MAX_FACTS);
      console.info('OHOS_Memory: fact added, count=' + p.facts.length);
    } else if (mutation.kind === 'set_preference') {
      if (p.preferences[mutation.key] === mutation.value) return false;
      p.preferences[mutation.key] = mutation.value;
      console.info('OHOS_Memory: preference updated, count=' + Object.keys(p.preferences).length);
    } else if (mutation.kind === 'add_topic') {
      for (let i: number = 0; i < p.topics.length; i++) if (p.topics[i] === mutation.value) return false;
      p.topics.push(mutation.value);
      if (p.topics.length > MAX_TOPICS) p.topics = p.topics.slice(-MAX_TOPICS);
    } else if (mutation.kind === 'increment') {
      p.conversationCount++;
    }
    p.updatedAt = new Date().toISOString();
    return true;
  }

  private static _normalizeProfile(value: UserProfile): UserProfile {
    return {
      preferences: value.preferences || {},
      facts: value.facts || [],
      topics: value.topics || [],
      updatedAt: value.updatedAt || new Date().toISOString(),
      conversationCount: value.conversationCount || 0,
    };
  }

  private static _cloneProfile(value: UserProfile): UserProfile {
    const preferences: Record<string, string> = {};
    const keys: string[] = Object.keys(value.preferences);
    for (let i: number = 0; i < keys.length; i++) preferences[keys[i]] = value.preferences[keys[i]];
    return {
      preferences,
      facts: value.facts.slice(),
      topics: value.topics.slice(),
      updatedAt: value.updatedAt,
      conversationCount: value.conversationCount,
    };
  }

  private static _startCleanupTimer(): void {
    if (this._cleanupTimer >= 0) return;
    // 每 5 分钟清理一次
    this._cleanupTimer = setInterval((): void => {
      this.cleanupExpiredSessions();
    }, 5 * 60 * 1000);
  }

  /**
   * 从对话文本中提取关键事实（简易版：基于规则）
   * 后续可以升级为 LLM 自动提取
   */
  static extractFactsFromConversation(userMsg: string, _assistantMsg: string): void {
    const extraction: StructuredMemoryExtraction = extractStructuredMemory(userMsg);
    if (extraction.fields.length === 0 && extraction.liked.length === 0 &&
        extraction.disliked.length === 0 && extraction.facts.length === 0 &&
        extraction.topics.length === 0) return;
    this._readyPromise.then((): void => {
      this._applyStructuredExtraction(extraction);
    }).catch((e: Error): void => {
      console.error('OHOS_Memory: structured extraction apply failed=' + String(e));
    });
  }

  private static _applyStructuredExtraction(extraction: StructuredMemoryExtraction): void {
    const p: UserProfile = this.getProfile();
    let changed: boolean = false;

    for (let i: number = 0; i < extraction.fields.length; i++) {
      const field: MemoryFieldUpdate = extraction.fields[i];
      if (p.preferences[field.key] !== field.value) {
        p.preferences[field.key] = field.value;
        changed = true;
      }
      if (field.key === '称呼') changed = this._removeLegacyFacts(p, ['我叫', '我姓']) || changed;
      else if (field.key === '所在地') changed = this._removeLegacyFacts(p, ['我来自', '我住在']) || changed;
      else if (field.key === '职业') changed = this._removeLegacyFacts(p, ['我是一个', '我是一名']) || changed;
      else if (field.key === '年龄') changed = this._removeLegacyFacts(p, ['我今年', '我已经']) || changed;
    }

    // Positive preferences are applied before negative ones, so an explicit
    // “不喜欢/不再喜欢” wins if both forms occur in one utterance.
    for (let i: number = 0; i < extraction.liked.length; i++) {
      const value: string = extraction.liked[i];
      changed = this._addPreferenceValue(p, '兴趣偏好', value) || changed;
      changed = this._removePreferenceValue(p, '不喜欢', value) || changed;
      changed = this._removeLegacyFacts(p, ['用户喜欢：' + value, '我喜欢' + value]) || changed;
    }
    for (let i: number = 0; i < extraction.disliked.length; i++) {
      const value: string = extraction.disliked[i];
      changed = this._removePreferenceValue(p, '兴趣偏好', value) || changed;
      changed = this._addPreferenceValue(p, '不喜欢', value) || changed;
      changed = this._removeLegacyFacts(p, ['用户喜欢：' + value, '我喜欢' + value]) || changed;
    }

    for (let i: number = 0; i < extraction.facts.length; i++) {
      if (p.facts.indexOf(extraction.facts[i]) >= 0) continue;
      p.facts.push(extraction.facts[i]);
      changed = true;
    }
    if (p.facts.length > MAX_FACTS) p.facts = p.facts.slice(-MAX_FACTS);

    for (let i: number = 0; i < extraction.topics.length; i++) {
      if (p.topics.indexOf(extraction.topics[i]) >= 0) continue;
      p.topics.push(extraction.topics[i]);
      changed = true;
    }
    if (p.topics.length > MAX_TOPICS) p.topics = p.topics.slice(-MAX_TOPICS);

    if (!changed) return;
    p.updatedAt = new Date().toISOString();
    this._saveProfile();
    console.info('OHOS_Memory: structured memory updated fields=' + Object.keys(p.preferences).length +
      ' facts=' + p.facts.length + ' topics=' + p.topics.length);
  }

  private static _preferenceValues(p: UserProfile, key: string): string[] {
    const value: string = p.preferences[key] || '';
    if (!value) return [];
    const raw: string[] = value.split(/[、,，]/);
    const result: string[] = [];
    for (let i: number = 0; i < raw.length; i++) {
      const item: string = raw[i].trim();
      if (item && result.indexOf(item) < 0) result.push(item);
    }
    return result;
  }

  private static _addPreferenceValue(p: UserProfile, key: string, value: string): boolean {
    const values: string[] = this._preferenceValues(p, key);
    if (values.indexOf(value) >= 0) return false;
    values.push(value);
    p.preferences[key] = values.join('、');
    return true;
  }

  private static _removePreferenceValue(p: UserProfile, key: string, value: string): boolean {
    const values: string[] = this._preferenceValues(p, key);
    const index: number = values.indexOf(value);
    if (index < 0) return false;
    values.splice(index, 1);
    if (values.length > 0) p.preferences[key] = values.join('、');
    else delete p.preferences[key];
    return true;
  }

  private static _removeLegacyFacts(p: UserProfile, markers: string[]): boolean {
    const before: number = p.facts.length;
    p.facts = p.facts.filter((fact: string): boolean => {
      for (let i: number = 0; i < markers.length; i++) {
        if (fact.indexOf(markers[i]) >= 0) return false;
      }
      return true;
    });
    return before !== p.facts.length;
  }
}
