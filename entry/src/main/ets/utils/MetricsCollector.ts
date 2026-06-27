/**
 * MetricsCollector.ts - ArkTS 侧性能指标采集器
 * 
 * 功能：
 * 1. LLM 响应时间自动埋点
 * 2. 消息统计（总对话数、总消息数）
 * 3. 调用 C++ MetricsCollector 的便捷封装
 * 
 * 使用方式：
 *   import { MetricsCollector } from '../utils/MetricsCollector';
 *   MetricsCollector.recordLlmLatency(1234);  // 记录 LLM 延迟
 *   MetricsCollector.recordMessage('user');     // 记录消息
 * 
 * 扩展方式（以"卡顿检测"为例）：
 *   1. 在 native_lib.d.ts 的 MetricTypeId 中添加 JankFrameDuration
 *   2. 在此类中添加 recordJank(durationMs) 方法
 *   3. 在检测到卡顿的地方调用
 */

import nativeLib from 'libnative_lib.so';

// ===== 指标类型 ID（与 C++ MetricsData.h 同步） =====
export const MetricTypeId = {
  AsrRtf: 0,
  AsrDecodeLatencyMs: 1,
  VadLatencyMs: 2,
  VadSpeechDurationMs: 3,
  TtsGenerationMs: 4,
  TtsAudioLengthMs: 5,
  RingBufferFillRate: 6,
  AudioChunkSize: 7,
  LlmLatencyMs: 8,
} as const;

// ===== 统计计数（仅 ArkTS 侧维护） =====
let _totalMessages: number = 0;
let _totalConversations: number = 0;
let _llmCallCount: number = 0;
let _llmTotalLatency: number = 0;

export class MetricsCollector {
  private static _initialized: boolean = false;
  private static _storagePath: string = '';

  /**
   * 初始化存储路径（由 Index.ets 在 aboutToAppear 中调用）
   */
  static init(filesDir: string): void {
    if (this._initialized) return;
    this._storagePath = filesDir + '/perf';
    nativeLib.initMetricsStorage(filesDir);
    this._initialized = true;
    console.info('OHOS_Metrics: MetricsCollector initialized at ' + this._storagePath);
  }

  /**
   * 启用/禁用开发者模式
   */
  static setEnabled(enabled: boolean): void {
    nativeLib.enableDevMetrics(enabled);
    console.info('OHOS_Metrics: ' + (enabled ? 'enabled' : 'disabled'));
  }

  // ======================== 便捷埋点 ========================

  /**
   * 记录 ASR RTF（实时因子）
   */
  static recordAsrRtf(rtf: number): void {
    nativeLib.recordMetric(MetricTypeId.AsrRtf, rtf);
  }

  /**
   * 记录 LLM 响应延迟 (ms)
   */
  static recordLlmLatency(ms: number): void {
    nativeLib.recordMetric(MetricTypeId.LlmLatencyMs, ms);
    _llmCallCount++;
    _llmTotalLatency += ms;
  }

  /**
   * 记录 TTS 生成耗时 (ms)
   */
  static recordTtsGeneration(ms: number): void {
    nativeLib.recordMetric(MetricTypeId.TtsGenerationMs, ms);
  }

  // ======================== 统计计数 ========================

  /**
   * 记录消息（用于统计）
   */
  static recordMessage(from: 'user' | 'assistant'): void {
    _totalMessages++;
  }

  /**
   * 开始新对话
   */
  static recordNewConversation(): void {
    _totalConversations++;
  }

  // ======================== 查询接口 ========================

  static getSummary(typeId: number): Object {
    return nativeLib.getMetricSummary(typeId);
  }

  static getTimeSeries(typeId: number, sinceMs?: number): Object {
    return nativeLib.getMetricTimeSeries(typeId, sinceMs);
  }

  static getLatestPoints(typeId: number, count: number): Object {
    return nativeLib.getLatestMetricPoints(typeId, count);
  }

  /**
   * 获取最新值（简化版，直接返回 number，避免 ArkTS 对象访问问题）
   */
  static getLatest(typeId: number): number {
    return nativeLib.getMetricLatest(typeId);
  }

  static getStats(): Object {
    return {
      'totalMessages': _totalMessages,
      'totalConversations': _totalConversations,
      'llmCallCount': _llmCallCount,
      'llmAvgLatency': _llmCallCount > 0 ? _llmTotalLatency / _llmCallCount : 0,
    };
  }

  // ======================== 管理接口 ========================

  static resetAll(): void {
    nativeLib.resetMetrics();
    _totalMessages = 0;
    _totalConversations = 0;
    _llmCallCount = 0;
    _llmTotalLatency = 0;
  }

  static exportJson(): string {
    return nativeLib.exportMetricsJson();
  }

  static isInitialized(): boolean {
    return MetricsCollector._initialized;
  }
}
