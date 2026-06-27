/**
 * DocumentManager.ts - 本地文档管理与关键词检索（轻量 RAG）
 *
 * 功能：
 * 1. 导入文档（.txt）到本地沙箱
 * 2. 构建关键词倒排索引
 * 3. 用户提问时关键词检索相关文档
 * 4. 检索结果注入 LLM Prompt
 *
 * 架构：
 *   documents/
 *     {id}.txt          ← 原始文档
 *   index/
 *     inverted.json     ← 关键词倒排索引
 *     meta.json         ← 文档元数据
 */

import { fileIo } from '@kit.CoreFileKit';

// ===== 数据结构 =====

interface DocMeta {
  id: string;
  fileName: string;
  title: string;
  importedAt: string;
  wordCount: number;
}

// 倒排索引：keyword → doc IDs
interface InvertedIndex {
  [keyword: string]: string[];
}

interface SearchResult {
  docId: string;
  title: string;
  snippet: string;
  score: number;
}

// ===== 常量 =====

const MAX_SNIPPET_LEN: number = 200;
const MAX_RESULTS: number = 3;
const MAX_DOC_SIZE: number = 1024 * 1024; // 1MB 单文件上限

// ============================================================
// DocumentManager
// ============================================================

export class DocumentManager {
  private static _baseDir: string = '';
  private static _docDir: string = '';
  private static _indexDir: string = '';
  private static _index: InvertedIndex = {};
  private static _metas: DocMeta[] = [];
  private static _initialized: boolean = false;

  /**
   * 初始化
   */
  static init(filesDir: string): void {
    if (this._initialized) return;
    this._baseDir = filesDir;
    this._docDir = filesDir + '/documents';
    this._indexDir = filesDir + '/index';
    try { fileIo.mkdirSync(this._docDir, true); } catch (_e) {}
    try { fileIo.mkdirSync(this._indexDir, true); } catch (_e) {}
    this._loadIndex();
    this._initialized = true;
    console.info('OHOS_RAG: DocumentManager initialized, docs=' + this._metas.length);
  }

  // ======================== 文档导入 ========================

  /**
   * 导入文档
   * @param fileName 原始文件名（用于显示）
   * @param content 文本内容
   * @returns 文档 ID
   */
  static importDocument(fileName: string, content: string): string {
    this._ensureInit();
    if (content.length > MAX_DOC_SIZE) {
      content = content.substring(0, MAX_DOC_SIZE);
    }
    const id: string = Date.now().toString(36) + '_' + Math.random().toString(36).substring(2, 6);
    const meta: DocMeta = {
      id, fileName,
      title: fileName.replace(/\.\w+$/, ''),
      importedAt: new Date().toISOString(),
      wordCount: content.length,
    };
    // 保存文档
    const docPath: string = this._docDir + '/' + id + '.txt';
    const file = fileIo.openSync(docPath, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
    fileIo.writeSync(file.fd, content);
    fileIo.closeSync(file);
    // 更新索引
    this._metas.push(meta);
    this._indexDocument(id, content);
    this._saveIndex();
    console.info('OHOS_RAG: imported "' + fileName + '" (' + content.length + ' chars) id=' + id);
    return id;
  }

  /**
   * 导入文档（从 ArrayBuffer，用于文件选择器）
   */
  static importDocumentFromBuffer(fileName: string, buf: ArrayBuffer): string {
    const content: string = this._bufferToString(buf);
    return this.importDocument(fileName, content);
  }

  /**
   * 删除文档
   */
  static deleteDocument(docId: string): void {
    this._ensureInit();
    // 删除文件
    try { fileIo.unlinkSync(this._docDir + '/' + docId + '.txt'); } catch (_e) {}
    // 删除元数据
    this._metas = this._metas.filter((m: DocMeta): boolean => m.id !== docId);
    // 重建索引
    this._rebuildIndex();
    this._saveIndex();
    console.info('OHOS_RAG: deleted doc ' + docId);
  }

  /**
   * 获取所有文档列表
   */
  static getDocuments(): DocMeta[] {
    this._ensureInit();
    return this._metas;
  }

  /**
   * 获取文档内容
   */
  static getDocumentContent(docId: string): string {
    this._ensureInit();
    try {
      return fileIo.readTextSync(this._docDir + '/' + docId + '.txt');
    } catch (_e) {
      return '';
    }
  }

  // ======================== 关键词检索 ========================

  /**
   * 从用户问题中提取关键词
   */
  static extractKeywords(query: string): string[] {
    // 中文分词简单实现：按常见分隔符拆分，过滤停用词和过短的词
    const tokens: string[] = query.split(/[，。！？、；：""''（）\s,.;:!?()]+/);
    const keywords: string[] = [];
    const stopWords: Set<string> = new Set([
      '的', '了', '是', '在', '我', '有', '和', '就', '不', '人', '都', '一',
      '一个', '上', '也', '很', '到', '说', '要', '去', '你', '会', '着',
      '没有', '看', '好', '自己', '这', '他', '她', '它', '们', '那', '些',
      '什么', '怎么', '如何', '为什么', '请', '帮', '问', '想', '知道',
      '吗', '吧', '呢', '啊', '哦', '嗯', '呀',
    ]);
    for (let i: number = 0; i < tokens.length; i++) {
      const t: string = tokens[i].trim();
      if (t.length < 2) continue;            // 过滤单字
      if (stopWords.has(t)) continue;         // 过滤停用词
      // 检查是否已存在（去重）
      let dup: boolean = false;
      for (let j: number = 0; j < keywords.length; j++) {
        if (keywords[j] === t) { dup = true; break; }
      }
      if (!dup) keywords.push(t);
      // 对长词尝试提取双字词（提高召回）
      if (t.length >= 4) {
        for (let j: number = 0; j < t.length - 1; j++) {
          const bigram: string = t.substring(j, j + 2);
          if (!stopWords.has(bigram) && bigram.length === 2) {
            let dup2: boolean = false;
            for (let k: number = 0; k < keywords.length; k++) {
              if (keywords[k] === bigram) { dup2 = true; break; }
            }
            if (!dup2) keywords.push(bigram);
          }
        }
      }
    }
    return keywords;
  }

  /**
   * 检索相关文档
   * @param query 用户问题
   * @returns 排序后的检索结果
   */
  static search(query: string): SearchResult[] {
    this._ensureInit();
    const keywords: string[] = this.extractKeywords(query);
    if (keywords.length === 0) return [];

    // 统计每个文档的匹配关键词数
    const scores: Record<string, number> = {};
    const matchedKeywords: Record<string, string[]> = {};

    for (let i: number = 0; i < keywords.length; i++) {
      const kw: string = keywords[i];
      const docIds: string[] | undefined = this._index[kw];
      if (!docIds) continue;
      for (let j: number = 0; j < docIds.length; j++) {
        const did: string = docIds[j];
        scores[did] = (scores[did] || 0) + 1;
        if (!matchedKeywords[did]) matchedKeywords[did] = [];
        matchedKeywords[did].push(kw);
      }
    }

    // 排序
    const sorted: string[] = Object.keys(scores).sort((a: string, b: string): number => {
      return (scores[b] || 0) - (scores[a] || 0);
    });

    // 取 top-N 并生成摘要
    const results: SearchResult[] = [];
    for (let i: number = 0; i < Math.min(sorted.length, MAX_RESULTS); i++) {
      const did: string = sorted[i];
      const meta: DocMeta | undefined = this._findMeta(did);
      if (!meta) continue;
      const content: string = this.getDocumentContent(did);
      const snippet: string = this._makeSnippet(content, matchedKeywords[did] || keywords);
      results.push({
        docId: did,
        title: meta.title,
        snippet,
        score: scores[did] || 0,
      });
    }
    return results;
  }

  /**
   * 生成用于 LLM 的上下文文本
   */
  static buildContext(query: string): string {
    const results: SearchResult[] = this.search(query);
    if (results.length === 0) return '';
    let ctx: string = '以下是检索到的相关文档内容（用于回答用户问题）：\n\n';
    for (let i: number = 0; i < results.length; i++) {
      ctx += '--- 文档：' + results[i].title + ' ---\n';
      ctx += results[i].snippet + '\n\n';
    }
    ctx += '请基于以上文档内容回答用户的问题。如果文档中没有相关信息，请如实告知。';
    console.info('OHOS_RAG: context built from ' + results.length + ' docs, len=' + ctx.length);
    return ctx;
  }

  // ======================== 内部方法 ========================

  private static _ensureInit(): void {
    if (!this._initialized) {
      throw new Error('DocumentManager not initialized');
    }
  }

  private static _findMeta(docId: string): DocMeta | undefined {
    for (let i: number = 0; i < this._metas.length; i++) {
      if (this._metas[i].id === docId) return this._metas[i];
    }
    return undefined;
  }

  /**
   * 为单个文档构建索引
   */
  private static _indexDocument(docId: string, content: string): void {
    const words: string[] = this._segment(content);
    for (let i: number = 0; i < words.length; i++) {
      const w: string = words[i];
      if (w.length < 2) continue;
      if (!this._index[w]) this._index[w] = [];
      // 去重
      let exists: boolean = false;
      for (let j: number = 0; j < this._index[w].length; j++) {
        if (this._index[w][j] === docId) { exists = true; break; }
      }
      if (!exists) this._index[w].push(docId);
    }
  }

  /**
   * 重建全部索引
   */
  private static _rebuildIndex(): void {
    this._index = {};
    for (let i: number = 0; i < this._metas.length; i++) {
      const content: string = this.getDocumentContent(this._metas[i].id);
      if (content) this._indexDocument(this._metas[i].id, content);
    }
  }

  /**
   * 简单中文分词（逐字 + 双字组合）
   */
  private static _segment(text: string): string[] {
    const result: string[] = [];
    // 先按标点/空格拆分
    const parts: string[] = text.split(/[，。！？、；：""''（）\s\[\]【】\r\n\t,.;:!?()]+/);
    for (let p: number = 0; p < parts.length; p++) {
      const part: string = parts[p].trim();
      if (!part) continue;
      // 加入完整词
      result.push(part);
      // 对中文长词提取双字组合
      if (part.length >= 3) {
        for (let i: number = 0; i < part.length - 1; i++) {
          const bigram: string = part.substring(i, i + 2);
          if (bigram.length === 2) result.push(bigram);
        }
      }
    }
    return result;
  }

  /**
   * 生成摘要（取匹配关键词附近的文本）
   */
  private static _makeSnippet(content: string, keywords: string[]): string {
    // 找第一个匹配关键词的位置
    let bestPos: number = 0;
    for (let k: number = 0; k < keywords.length; k++) {
      const pos: number = content.indexOf(keywords[k]);
      if (pos >= 0) { bestPos = pos; break; }
    }
    const start: number = Math.max(0, bestPos - 60);
    const end: number = Math.min(content.length, start + MAX_SNIPPET_LEN);
    let snippet: string = content.substring(start, end);
    if (start > 0) snippet = '…' + snippet;
    if (end < content.length) snippet = snippet + '…';
    return snippet;
  }

  private static _saveIndex(): void {
    try {
      const data: string = JSON.stringify({
        index: this._index,
        metas: this._metas,
      });
      const path: string = this._indexDir + '/inverted.json';
      const file = fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(file.fd, data);
      fileIo.closeSync(file);
    } catch (e) {
      console.error('OHOS_RAG: save index failed: ' + String(e));
    }
  }

  private static _loadIndex(): void {
    try {
      const path: string = this._indexDir + '/inverted.json';
      const data: string = fileIo.readTextSync(path);
      const parsed: Record<string, Object> = JSON.parse(data) as Record<string, Object>;
      this._index = (parsed['index'] as InvertedIndex) || {};
      this._metas = (parsed['metas'] as DocMeta[]) || [];
      console.info('OHOS_RAG: loaded index, ' + Object.keys(this._index).length + ' keywords');
    } catch (_e) {
      this._index = {};
      this._metas = [];
    }
  }

  private static _bufferToString(buf: ArrayBuffer): string {
    const uint8: Uint8Array = new Uint8Array(buf);
    let result: string = '';
    for (let i: number = 0; i < uint8.length; i++) {
      result += String.fromCharCode(uint8[i]);
    }
    return result;
  }
}
