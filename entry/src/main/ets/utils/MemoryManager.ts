/**
 * MemoryManager.ts - 混合记忆管理器
 *
 * 管理两层记忆：
 * 1. 短期记忆（session）：当前对话上下文，超时自动清理
 * 2. 长期记忆（profile）：用户画像 + 已知事实，持久化到 JSON 文件
 */

import { fileIo } from '@kit.CoreFileKit';
import { CryptoManager } from './CryptoManager';
import { EncryptedStore } from './EncryptedStore';

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
  private static _sessions: Map<string, SessionState> = new Map();
  private static _cleanupTimer: number = -1;

  /**
   * 初始化（由 Index.ets 在 aboutToAppear 中调用）
   */
  static init(filesDir: string): void {
    if (this._initialized) return;
    this._storagePath = filesDir + '/memory';
    try { fileIo.mkdirSync(this._storagePath, true); } catch (_e) {}
    this._profile = this._loadProfile();
    this._startCleanupTimer();
    this._initialized = true;
    console.info('OHOS_Memory: MemoryManager initialized');
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
    const p: UserProfile = this.getProfile();
    // 去重：不添加完全相同的事实
    for (let i: number = 0; i < p.facts.length; i++) {
      if (p.facts[i] === fact) return;
    }
    p.facts.push(fact);
    if (p.facts.length > MAX_FACTS) {
      p.facts = p.facts.slice(-MAX_FACTS);
    }
    p.updatedAt = new Date().toISOString();
    this._saveProfile();
    console.info('OHOS_Memory: added fact: ' + fact);
  }

  /**
   * 设置偏好
   */
  static setPreference(key: string, value: string): void {
    const p: UserProfile = this.getProfile();
    p.preferences[key] = value;
    p.updatedAt = new Date().toISOString();
    this._saveProfile();
    console.info('OHOS_Memory: set preference: ' + key + '=' + value);
  }

  /**
   * 添加话题
   */
  static addTopic(topic: string): void {
    const p: UserProfile = this.getProfile();
    for (let i: number = 0; i < p.topics.length; i++) {
      if (p.topics[i] === topic) return;
    }
    p.topics.push(topic);
    if (p.topics.length > MAX_TOPICS) {
      p.topics = p.topics.slice(-MAX_TOPICS);
    }
    p.updatedAt = new Date().toISOString();
    this._saveProfile();
  }

  /**
   * 增加对话计数
   */
  static incrementConversationCount(): void {
    const p: UserProfile = this.getProfile();
    p.conversationCount++;
    p.updatedAt = new Date().toISOString();
    this._saveProfile();
  }

  /**
   * 重置所有记忆
   */
  static resetAll(): void {
    this._profile = defaultProfile();
    this._sessions.clear();
    this._saveProfile();
    console.info('OHOS_Memory: reset all memories');
  }

  // ======================== 内部方法 ========================

  private static _profilePath(): string {
    return this._storagePath + '/profile.json';
  }

  private static _loadProfile(): UserProfile {
    try {
      const content: string = fileIo.readTextSync(this._profilePath());
      const data: Record<string, Object> = JSON.parse(content) as Record<string, Object>;
      // 兼容旧格式，确保字段都存在
      return {
        preferences: (data['preferences'] as Record<string, string>) || {},
        facts: (data['facts'] as string[]) || [],
        topics: (data['topics'] as string[]) || [],
        updatedAt: (data['updatedAt'] as string) || new Date().toISOString(),
        conversationCount: (data['conversationCount'] as number) || 0,
      };
    } catch (_e) {
      return defaultProfile();
    }
  }

  private static _saveProfile(): void {
    if (!this._profile) return;
    try {
      const json: string = JSON.stringify(this._profile, null, 2);
      const path: string = this._profilePath();
      const file = fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(file.fd, json);
      fileIo.closeSync(file);
      // 异步加密备份
      EncryptedStore.write(path + '.enc', this._profile);
    } catch (e) {
      console.error('OHOS_Memory: save profile failed: ' + String(e));
    }
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
  static extractFactsFromConversation(userMsg: string, assistantMsg: string): void {
    // 简单规则：检测常见的自我介绍和偏好表达
    const rules: RegExp[] = [
      /我(姓|叫|是)(.{1,6})[,，。！\s]/,
      /我(住在|来自|在)(.{1,10})(工作|生活|上学|住)/,
      /我(喜欢|热爱|爱好|最喜欢)(.{1,20})[,，。！\s]/,
      /我是(一[个名]).{1,10}(工程师|设计师|学生|老师|医生|程序员|产品|运营|自由职业)/,
      /我有(.{1,10})(只|条|个|台)(猫|狗|宠物|车)/,
      /我(今年|已经).{1,5}(岁|岁啦|岁了)/,
      /(对|关于).{0,10}(敏感|过敏|不喜欢|讨厌)(.{1,20})/,
    ];

    for (let i: number = 0; i < rules.length; i++) {
      const match: RegExpExecArray | null = rules[i].exec(userMsg);
      if (match) {
        const fact: string = '用户提到：' + match[0].replace(/[,，。！\s]+$/, '');
        this.addFact(fact);
      }
    }

    // 检测话题
    const topicKeywords: string[][] = [
      ['天气', '下雨', '下雪', '温度', '台风'],
      ['编程', '代码', '程序', '开发', '软件', '算法'],
      ['健康', '跑步', '运动', '健身', '瑜伽', '体检'],
      ['美食', '做饭', '菜谱', '餐厅', '吃饭'],
      ['旅行', '旅游', '出差', '机票', '酒店'],
      ['音乐', '电影', '读书', '小说', '游戏'],
    ];

    for (let i: number = 0; i < topicKeywords.length; i++) {
      for (let j: number = 0; j < topicKeywords[i].length; j++) {
        if (userMsg.indexOf(topicKeywords[i][j]) >= 0) {
          this.addTopic(topicKeywords[i][0]);
          break;
        }
      }
    }
  }
}
