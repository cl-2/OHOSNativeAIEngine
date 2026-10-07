type TtsCallback = (data: ArrayBuffer, progress: number, sampleRate: number, generation: number,
  sampleCount?: number) => void;
type AsrCallback = (text: string, isFinal: boolean) => void;
type PerfCallback = (currentRtf: number, avgRtf: number, maxRtf: number, latencyMs: number, vadLatencyMs: number, needsDegradation: boolean) => void;

interface NativeLibModule {
  pingNative(): number;
  // 生成性能体检报告（含内存 + 各指标统计）
  getBenchmarkReport(): string;
  // 运行性能基准测试（内存 + Pipeline 压测），返回 JSON
  runBenchmark(): string;
  startStreamingTtsWithSpeed(text: string, modelDir: string, speed: number, callback: TtsCallback): void;
  setTtsNumThreads(numThreads: number): number;
  stopTts(): void;
  releaseTtsEngine(): boolean;
  beginTtsLifecycleLease(): void;
  endTtsLifecycleLease(): void;
  ackTtsChunk(sampleCount: number, generation: number): void;
  startAssistant(asrModelDir: string, vadModelPath: string, kwsModelPath: string, callback: AsrCallback): void;
  setPerfCallback(callback: PerfCallback): void;
  feedAudio(pcmData: ArrayBuffer): void;
  stopAssistant(): void;
  // RawFile API
  initRawfileMgmt(abilityContext: Object): boolean;
  readRawFileSync(rawPath: string): ArrayBuffer | undefined;
  // 流式复制 rawfile 到沙箱（C++ 侧分块读写，安全处理大文件）
  copyRawFile(rawPath: string, destPath: string): boolean;
  // 全双工音频状态机
  startFullDuplex(): void;
  stopFullDuplex(): void;
  getFullDuplexState(): string;
  setDuplexCallbacks(callback: (action: string, payload: string) => void): void;
  notifyDuplexEvent(event: string, payload?: string): void;
  checkInterruption(): boolean;
  resetAsrStream(): void;
  // 本地 LLM（Qwen2.5-0.5B + ONNX Runtime）
  initLocalLlm(modelDir: string): boolean;
  callLocalLlm(text: string, historyJson: string, onToken: (token: string) => void, onComplete: (text: string, success: boolean) => void): void;
  setLocalLlmDiagnosticTokens(tokens: number): number;
  isLocalLlmAvailable(modelDir?: string): boolean;
  releaseLocalLlm(): void;
  getRuntimeMemorySnapshot(): string;
  runVoiceLogicRegression(): string;
  // 性能监控
  enableDevMetrics(enabled: boolean): void;
  initMetricsStorage(filesDir: string): void;
  recordMetric(typeId: number, value: number): void;
  getMetricTimeSeries(typeId: number, sinceMs?: number): Object;
  getMetricSummary(typeId: number): Object;
  getLatestMetricPoints(typeId: number, count: number): Object[];
  getMetricLatest(typeId: number): number;
  resetMetrics(): void;
  exportMetricsJson(): string;
}

declare const nativeLib: NativeLibModule;
export default nativeLib;
