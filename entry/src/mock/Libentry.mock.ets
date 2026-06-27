const NativeMock = {
  pingNative: (): number => {
    return 1;
  },
  startStreamingTtsWithSpeed: (_text: string, _modelDir: string, _speed: number, callback: Function): void => {
    if (callback) {
      callback(new ArrayBuffer(0), 1.0, 24000);
    }
  },
  stopTts: (): void => {
  },
  startAssistant: (_asrModelDir: string, _vadModelPath: string, _kwsModelPath: string, callback: Function): void => {
    // mock: 模拟ASR回调，返回测试文本
    if (callback) {
      callback('测试识别结果', false);
    }
  },
  setPerfCallback: (_callback: Function): void => {
    // mock: 性能回调已注册
  },
  feedAudio: (_buffer: ArrayBuffer): void => {
    // mock: 音频数据已喂入
  },
  stopAssistant: (): void => {
    // mock: ASR已停止
  },
};

export default NativeMock;