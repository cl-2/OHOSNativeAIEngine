type TtsCallback = (data: ArrayBuffer, progress: number, sampleRate: number) => void;
type AsrCallback = (text: string, isFinal: boolean) => void;
type PerfCallback = (currentRtf: number, avgRtf: number, maxRtf: number, latencyMs: number, vadLatencyMs: number, needsDegradation: boolean) => void;

interface NativeLibModule {
  pingNative(): number;
  startStreamingTtsWithSpeed(text: string, modelDir: string, speed: number, callback: TtsCallback): void;
  stopTts(): void;
  startAssistant(asrModelDir: string, vadModelPath: string, kwsModelPath: string, callback: AsrCallback): void;
  setPerfCallback(callback: PerfCallback): void;
  feedAudio(pcmData: ArrayBuffer): void;
  stopAssistant(): void;
  // RawFile API
  initRawfileMgmt(abilityContext: Object): boolean;
  readRawFileSync(rawPath: string): ArrayBuffer | undefined;
  // 全双工音频状态机
  startFullDuplex(): void;
  stopFullDuplex(): void;
  getFullDuplexState(): string;
  setDuplexCallbacks(callback: (action: string, payload: string) => void): void;
  notifyDuplexEvent(event: string, payload?: string): void;
  checkInterruption(): boolean;
  resetAsrStream(): void;
  // 本地 LLM（Qwen2.5-0.5B + ONNX Runtime）
  callLocalLlm(text: string, historyJson: string, onToken: (token: string) => void, onComplete: (text: string, success: boolean) => void): void;
  isLocalLlmAvailable(): boolean;
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
