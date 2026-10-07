import worker, { ThreadWorkerGlobalScope, MessageEvents } from '@ohos.worker';
import nativeLib from 'libnative_lib.so';
import audio from '@ohos.multimedia.audio';

console.info('OHOS_Native_ai_engine: AsrWorker loaded');

let audioCapturer: audio.AudioCapturer | null = null;
let isRunning: boolean = false;
let isInitializing: boolean = false;  // 防止并发初始化
let offlineMode: boolean = false;
let captureGeneration: number = 0;

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

// Some devices abort inside NapiCapturerReadDataCallback when the voice-
// communication route is reconfigured by TTS. Pull-mode read avoids that
// system callback path while retaining the platform AEC source.
async function runCaptureLoop(capturer: audio.AudioCapturer,
  bufferSize: number, generation: number): Promise<void> {
  while (isRunning && generation === captureGeneration) {
    try {
      const buffer: ArrayBuffer = await capturer.read(bufferSize, true);
      if (!isRunning || generation !== captureGeneration) break;
      if (buffer && buffer.byteLength > 0) nativeLib.feedAudio(buffer);
    } catch (err) {
      if (isRunning && generation === captureGeneration) {
        console.error('OHOS_Native_ai_engine: capturer read failed: ' + String(err));
        workerPort.postMessage({ msgType: 'asr-capture-error', error: String(err) });
      }
      break;
    }
  }
}

// 性能指标推送节流
let lastPerfReportTime: number = 0;

workerPort.onmessage = async (e: MessageEvents) => {
  const msgType: string = e.data['msgType'] as string;

  // 预热模式：在 Worker 内预加载 ASR 引擎
  if (msgType === 'prewarmAsr') {
    const asrModelDir: string = e.data['asrModelDir'] as string;
    const vadModelPath: string = e.data['vadModelPath'] as string;
    const startedAt: number = Date.now();
    console.info('OHOS_Native_ai_engine: AsrWorker prewarming ASR engine...');
    try {
      nativeLib.startAssistant(asrModelDir, vadModelPath, '__prewarm__', (): void => {});
      const durationMs: number = Date.now() - startedAt;
      console.info('OHOS_Native_ai_engine: AsrWorker prewarm done in ' + durationMs + 'ms');
      workerPort.postMessage({ msgType: 'asr-prewarm-done', success: true, durationMs });
    } catch (e) {
      workerPort.postMessage({
        msgType: 'asr-prewarm-done', success: false,
        durationMs: Date.now() - startedAt, error: String(e)
      });
    }
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
          // Voice communication route enables the device/system AEC path when supported.
          source: audio.SourceType.SOURCE_TYPE_VOICE_COMMUNICATION,
          capturerFlags: 0
        }
      };
      audioCapturer = await audio.createAudioCapturer(audioCapturerOptions);

      // 3. 使用主动读取循环，规避部分设备在通信路由重配置时的 readData 回调崩溃。
      const bufferSize: number = await audioCapturer.getBufferSize();

      // 4. 开始录音
      await audioCapturer.start();
      const generation: number = ++captureGeneration;
      runCaptureLoop(audioCapturer, bufferSize, generation);
      isInitializing = false;
    } catch (err) {
      const e2 = err as Error;
      const errMsg: string = e2.message || String(err);
      console.error('OHOS_Native_ai_engine: AsrWorker ERROR: ' + errMsg);
      console.error('OHOS_Native_ai_engine: AsrWorker stack: ' + (e2.stack || 'no stack'));
      isRunning = false;
      isInitializing = false;
      captureGeneration++;
      if (audioCapturer) {
        try { await audioCapturer.stop(); } catch (_stopErr) {}
        try { await audioCapturer.release(); } catch (_releaseErr) {}
        audioCapturer = null;
      }
      workerPort.postMessage({ msgType: 'asr-stopped', error: errMsg });
    }
  }

  if (msgType === 'stopAsr') {
    console.info('OHOS_Native_ai_engine: AsrWorker stopAssistant received');
    isRunning = false;
    isInitializing = false;
    captureGeneration++;
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
