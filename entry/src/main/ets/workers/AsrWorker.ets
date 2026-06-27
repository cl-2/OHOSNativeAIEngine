import worker, { ThreadWorkerGlobalScope, MessageEvents } from '@ohos.worker';
import nativeLib from 'libnative_lib.so';
import audio from '@ohos.multimedia.audio';

console.info('OHOS_Native_ai_engine: AsrWorker loaded');

let audioCapturer: audio.AudioCapturer | null = null;
let isRunning: boolean = false;
let isInitializing: boolean = false;  // 防止并发初始化
let offlineMode: boolean = false;

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

// 性能指标推送节流
let lastPerfReportTime: number = 0;

workerPort.onmessage = async (e: MessageEvents) => {
  const msgType: string = e.data['msgType'] as string;

  // 预热模式：在 Worker 内预加载 ASR 引擎
  if (msgType === 'prewarmAsr') {
    const asrModelDir: string = e.data['asrModelDir'] as string;
    const vadModelPath: string = e.data['vadModelPath'] as string;
    console.info('OHOS_Native_ai_engine: AsrWorker prewarming ASR engine...');
    nativeLib.startAssistant(asrModelDir, vadModelPath, '__prewarm__', (): void => {});
    console.info('OHOS_Native_ai_engine: AsrWorker prewarm done');
    return;
  }

  if (msgType === 'startAsr') {
    // 全双工模式：防止重复初始化
    if (audioCapturer !== null || isInitializing) {
      console.info('OHOS_Native_ai_engine: AsrWorker already running/initializing, skip duplicate startAsr');
      return;
    }
    isInitializing = true;

    const asrModelDir: string = e.data['asrModelDir'] as string;
    const vadModelPath: string = e.data['vadModelPath'] as string;
    const kwsModelPath: string = e.data['kwsModelPath'] as string;
    // 离线模式参数
    offlineMode = e.data['offlineMode'] === true;
    console.info('OHOS_Native_ai_engine: AsrWorker startAssistant, asrModelDir=' + asrModelDir +
      ', vadModelPath=' + vadModelPath + ', kwsModelPath=' + kwsModelPath +
      ', offlineMode=' + offlineMode);
    isRunning = true;

    try {
      // 1. 初始化 native Assistant 引擎
      console.info('OHOS_Native_ai_engine: AsrWorker calling native startAssistant...');
      nativeLib.startAssistant(asrModelDir, vadModelPath, kwsModelPath,
        // ASR结果回调
        (text: string, isFinal: boolean): void => {
          console.info('OHOS_Native_ai_engine: AsrWorker ASR result: "' + text + '", isFinal=' + isFinal);
          try {
            workerPort.postMessage({
              msgType: 'asr-result',
              text: text,
              isFinal: isFinal,
              offlineMode: offlineMode
            });
          } catch (postErr) {
            console.error('OHOS_Native_ai_engine: AsrWorker postMessage failed: ' + String(postErr));
          }
        });
      console.info('OHOS_Native_ai_engine: AsrWorker native Assistant engine initialized');

      // 1b. 单独注册性能监控回调
      console.info('OHOS_Native_ai_engine: AsrWorker registering perf callback...');
      nativeLib.setPerfCallback(
        (currentRtf: number, avgRtf: number, maxRtf: number,
         latencyMs: number, vadLatencyMs: number, needsDegradation: boolean): void => {
          try {
            // 节流：每300ms推送一次性能数据
            const now: number = Date.now();
            if (now - lastPerfReportTime < 300) return;
            lastPerfReportTime = now;

            workerPort.postMessage({
              msgType: 'perf-metrics',
              currentRtf: currentRtf,
              avgRtf: avgRtf,
              maxRtf: maxRtf,
              latencyMs: latencyMs,
              vadLatencyMs: vadLatencyMs,
              needsDegradation: needsDegradation,
              offlineMode: offlineMode
            });

            // RTF > 0.3 时自动触发降级通知
            if (needsDegradation && !offlineMode) {
              console.warn('OHOS_Native_ai_engine: AsrWorker RTF degradation triggered, switching to light model');
              workerPort.postMessage({
                msgType: 'degradation-triggered',
                reason: 'RTF=' + currentRtf.toFixed(3) + ' > 0.3 threshold'
              });
            }
          } catch (perfErr) {
            console.error('OHOS_Native_ai_engine: AsrWorker perf report failed: ' + String(perfErr));
          }
        });
      console.info('OHOS_Native_ai_engine: AsrWorker perf callback registered');

      // 2. 创建 AudioCapturer（麦克风采集 16kHz 单声道 int16）
      const audioStreamInfo: audio.AudioStreamInfo = {
        samplingRate: audio.AudioSamplingRate.SAMPLE_RATE_16000,
        channels: audio.AudioChannel.CHANNEL_1,
        sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
        encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW
      };
      const audioCapturerOptions: audio.AudioCapturerOptions = {
        streamInfo: audioStreamInfo,
        capturerInfo: {
          source: audio.SourceType.SOURCE_TYPE_MIC,
          capturerFlags: 0
        }
      };
      audioCapturer = await audio.createAudioCapturer(audioCapturerOptions);

      // 3. 注册录音数据回调
      const bufferSize: number = await audioCapturer.getBufferSize();

      let readDataCount: number = 0;
      audioCapturer.on('readData', (buffer: ArrayBuffer) => {
        readDataCount++;
        if (!isRunning) return;
        // 将麦克风 PCM 数据通过环形缓冲区喂入 native Assistant（无锁、零拷贝）
        nativeLib.feedAudio(buffer);
      });

      // 4. 开始录音
      await audioCapturer.start();
    } catch (err) {
      const e2 = err as Error;
      const errMsg: string = e2.message || String(err);
      console.error('OHOS_Native_ai_engine: AsrWorker ERROR: ' + errMsg);
      console.error('OHOS_Native_ai_engine: AsrWorker stack: ' + (e2.stack || 'no stack'));
      isRunning = false;
      isInitializing = false;
      workerPort.postMessage({ msgType: 'asr-stopped', error: errMsg });
    }
  }

  if (msgType === 'stopAsr') {
    console.info('OHOS_Native_ai_engine: AsrWorker stopAssistant received');
    isRunning = false;
    isInitializing = false;
    try {
      if (audioCapturer) {
        await audioCapturer.stop();
        await audioCapturer.release();
        audioCapturer = null;
        console.info('OHOS_Native_ai_engine: AsrWorker AudioCapturer released');
      }
    } catch (e) {
      console.error('OHOS_Native_ai_engine: AsrWorker error releasing capturer: ' + String(e));
    }
    nativeLib.stopAssistant();
    console.info('OHOS_Native_ai_engine: AsrWorker native Assistant stopped');
    workerPort.postMessage({ msgType: 'asr-stopped' });
  }
};
