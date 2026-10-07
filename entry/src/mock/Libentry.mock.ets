type MockTtsCallback = (data: ArrayBuffer, progress: number, sampleRate: number) => void;
type MockAsrCallback = (text: string, isFinal: boolean) => void;
type MockPerfCallback = (currentRtf: number, avgRtf: number, maxRtf: number,
  latencyMs: number, vadLatencyMs: number, needsDegradation: boolean) => void;

interface NativeMockModule {
  pingNative(): number;
  startStreamingTtsWithSpeed(text: string, modelDir: string, speed: number, callback: MockTtsCallback): void;
  stopTts(): void;
  startAssistant(asrModelDir: string, vadModelPath: string, kwsModelPath: string,
    callback: MockAsrCallback): void;
  setPerfCallback(callback: MockPerfCallback): void;
  feedAudio(buffer: ArrayBuffer): void;
  stopAssistant(): void;
}

class NativeMockModuleImpl implements NativeMockModule {
  pingNative(): number {
    return 1;
  }

  startStreamingTtsWithSpeed(_text: string, _modelDir: string, _speed: number,
    callback: MockTtsCallback): void {
    if (callback) {
      callback(new ArrayBuffer(0), 1.0, 24000);
    }
  }

  stopTts(): void {
  }

  startAssistant(_asrModelDir: string, _vadModelPath: string, _kwsModelPath: string,
    callback: MockAsrCallback): void {
    // mock: 模拟ASR回调，返回测试文本
    if (callback) {
      callback('测试识别结果', false);
    }
  }

  setPerfCallback(_callback: MockPerfCallback): void {
    // mock: 性能回调已注册
  }

  feedAudio(_buffer: ArrayBuffer): void {
    // mock: 音频数据已喂入
  }

  stopAssistant(): void {
    // mock: ASR已停止
  }
}

const NativeMock: NativeMockModule = new NativeMockModuleImpl();

export default NativeMock;
