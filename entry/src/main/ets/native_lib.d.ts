
type TtsCallback = (data: ArrayBuffer, progress: number, sampleRate: number) => void;
type AsrCallback = (text: string, isFinal: boolean) => void;

// 性能监控回调
// currentRtf: 当前RTF值
// avgRtf: 平均RTF值
// maxRtf: 峰值RTF值
// latencyMs: 最近一次端到端延迟(ms)
// vadLatencyMs: VAD处理延迟(ms)
// needsDegradation: 是否需要降级模型（RTF>0.3时触发）
type PerfCallback = (currentRtf: number, avgRtf: number, maxRtf: number, latencyMs: number, vadLatencyMs: number, needsDegradation: boolean) => void;

// ========== 性能监控类型 ==========
// 指标类型枚举（与 C++ MetricsData.h 同步）
const enum MetricTypeId {
  AsrRtf = 0,
  AsrDecodeLatencyMs = 1,
  VadLatencyMs = 2,
  VadSpeechDurationMs = 3,
  TtsGenerationMs = 4,
  TtsAudioLengthMs = 5,
  RingBufferFillRate = 6,
  AudioChunkSize = 7,
  LlmLatencyMs = 8,
}

interface MetricPoint {
  t: number;  // timestamp ms
  v: number;  // value
}

interface MetricSummary {
  avg: number;
  max: number;
  min: number;
  latest: number;
  count: number;
  p50: number;
  p95: number;
  p99: number;
}

interface MetricTimeSeriesResult {
  count: number;
  points: MetricPoint[];
}

// 硬件编解码器接口
interface HardwareCodec {
  // 不透明句柄，由 native 层管理生命周期
}

interface CodecOutputData {
  data: ArrayBuffer;
  pts: number;
  ok: boolean;
}

// 硬件编解码基准测试结果
interface CodecBenchmarkResult {
  codecName: string;
  mimeType: string;
  testDurationMs: number;
  totalFrames: number;
  decodedFrames: number;
  throughputFps: number;
  avgLatencyMs: number;
  minLatencyMs: number;
  maxLatencyMs: number;
  p95LatencyMs: number;
  p99LatencyMs: number;
}

interface NativeLibModule {
  pingNative(): number;
  prewarmTts(modelDir: string): void;
  startStreamingTtsWithSpeed(text: string, modelDir: string, speed: number, callback: TtsCallback): void;
  stopTts(): void;
  // startAssistant - 启动ASR引擎
  // 参数: (asrModelDir, vadModelPath, kwsModelPath, asrCallback)
  startAssistant(asrModelDir: string, vadModelPath: string, kwsModelPath: string, asrCallback: AsrCallback): void;
  // setPerfCallback - 单独注册性能监控回调
  setPerfCallback(perfCallback: PerfCallback): void;
  feedAudio(buffer: ArrayBuffer): void;
  stopAssistant(): void;

  // ========== 本地 LLM（Qwen2.5-0.5B + ONNX Runtime）==========
  callLocalLlm(text: string, historyJson: string, onToken: (token: string) => void, onComplete: (text: string, success: boolean) => void): void;
  isLocalLlmAvailable(): boolean;

  // ========== 硬件加速接口 (MediaCodec HAL) ==========
  // 创建硬件编解码器实例
  createHardwareCodec(): HardwareCodec;
  // 初始化编解码器: (codec, mimeType: string, isEncoder: boolean) => boolean
  initHardwareCodec(codec: HardwareCodec, mimeType: string, isEncoder: boolean): boolean;
  // 喂入原始数据: (codec, data: ArrayBuffer, pts: number) => boolean
  codecQueueInput(codec: HardwareCodec, data: ArrayBuffer, pts: number): boolean;
  // 取解码输出: (codec) => CodecOutputData | null
  codecDequeueOutput(codec: HardwareCodec): CodecOutputData;
  // 释放编解码器: (codec) => void
  releaseHardwareCodec(codec: HardwareCodec): void;

  // ========== 硬件编解码基准测试 ==========
  // 运行编解码性能基准测试
  // (mimeType: string, testDurationMs?: number) => CodecBenchmarkResult
  runCodecBenchmark(mimeType: string, testDurationMs?: number): CodecBenchmarkResult;

  // ========== 全双工音频状态机 ==========
  // 启动全双工模式 (开始打断检测)
  startFullDuplex(): void;
  // 停止全双工模式
  stopFullDuplex(): void;
  // 获取当前全双工状态
  getFullDuplexState(): string;
  // 设置 Action 回调（状态机 → ArkTS），回调参数 (action: string, payload: string)
  setDuplexCallbacks(callback: (action: string, payload: string) => void): void;
  // 通知状态机事件，事件列表: llm_start, llm_complete, tts_started, tts_complete, interrupt
  notifyDuplexEvent(event: string, payload?: string): void;

  // ========== Supertonic TTS 引擎 ==========
  // 初始化 Supertonic TTS (modelDir: string, voiceStyle: string) => boolean
  initSupertonicTts(modelDir: string, voiceStyle: string): boolean;
  // 切换声音风格 (voiceName: "M1" | "F1" 等) => boolean
  setSupertonicVoice(voiceName: string): boolean;
  // 获取可用声音风格列表 => string[]
  getSupertonicVoices(): string[];
  // 检查 Supertonic TTS 是否已初始化 => boolean
  supertonicTtsIsInitialized(): boolean;

  // ========== 性能监控接口 ==========
  // 启用/禁用开发者模式
  enableDevMetrics(enabled: boolean): void;
  // 初始化存储路径（需要 filesDir）
  initMetricsStorage(filesDir: string): void;
  // ArkTS 侧埋点：recordMetric(typeId, value)
  recordMetric(typeId: number, value: number): void;
  // 获取时序数据
  getMetricTimeSeries(typeId: number, sinceMs?: number): MetricTimeSeriesResult;
  // 获取统计摘要
  getMetricSummary(typeId: number): MetricSummary;
  // 获取最新 N 个数据点
  getLatestMetricPoints(typeId: number, count: number): MetricPoint[];
  // 获取最新值（简化版，直接返回 number）
  getMetricLatest(typeId: number): number;
  // 重置所有指标
  resetMetrics(): void;
  // 导出全部指标为 JSON 字符串
  exportMetricsJson(): string;
}

declare const nativeLib: NativeLibModule;
export default nativeLib;
