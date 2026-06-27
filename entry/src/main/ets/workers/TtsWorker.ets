import worker, { ThreadWorkerGlobalScope, MessageEvents } from '@ohos.worker';
import nativeLib from 'libnative_lib.so';
import audio from '@ohos.multimedia.audio';

console.info('OHOS_Native_ai_engine: TtsWorker loaded (streaming)');

let audioRenderer: audio.AudioRenderer | null = null;
let isPlaying: boolean = false;
let isStopping: boolean = false;
let ttsBufferSize: number = 0;
let rendererReady: boolean = false;
let pendingBuffer: ArrayBuffer[] | null = null;
let pendingDone: boolean = false;
let writeCount: number = 0;
let ttsSampleRate: number = 0;
let totalChunkBytes: number = 0;
let isFirstPlay: boolean = true; // 首帧播放标记，用于淡入
let ttsPendingCount: number = 0;  // 待处理的 TTS 句数
let ttsCompletedCount: number = 0; // 已完成的 TTS 句数

// 生成队列：保证多句串行生成（一条 async 协程逐个 await）
interface TtsTask {
  text: string;
  modelDir: string;
  speed: number;
}
const generationQueue: TtsTask[] = [];
let generationQueueRunning: boolean = false;

// 自适应缓冲参数
const MIN_START_MS: number = 200;    // 起播最小缓冲 (ms)
const MAX_BUF_MS: number = 5000;     // 最大缓冲 (ms)，超了丢弃旧包

// 写队列：保证串行写入，防电流音
const writeQueue: ArrayBuffer[] = [];
let writeQueueRunning: boolean = false;

async function processWriteQueue(): Promise<void> {
  if (writeQueueRunning) return;
  writeQueueRunning = true;
  while (writeQueue.length > 0 && !isStopping) {
    const data: ArrayBuffer = writeQueue.shift()!;
    await doWriteInternal(data);
    writeCount--;
  }
  writeQueueRunning = false;
  // 如果在处理期间又有新数据入队，重启处理
  if (writeQueue.length > 0 && !isStopping) {
    processWriteQueue();
    return;
  }
  if (pendingDone) checkAllDone();
}

async function doWriteInternal(data: ArrayBuffer): Promise<void> {
  if (!audioRenderer) { console.error('OHOS_ai_TTS: no audioRenderer, drop ' + data.byteLength + ' bytes'); return; }
  let offset: number = 0;
  while (offset < data.byteLength && !isStopping) {
    const remaining: number = data.byteLength - offset;
    const chunkSize: number = remaining < ttsBufferSize ? remaining : ttsBufferSize;
    const chunk: ArrayBuffer = data.slice(offset, offset + chunkSize);
    try {
      await audioRenderer.write(chunk);
    } catch (_e) {
      console.error('OHOS_ai_TTS_WRITE_ERR: ' + String(_e));
    }
    offset += chunkSize;
  }
}

function doWrite(data: ArrayBuffer): void {
  writeCount++;
  writeQueue.push(data);
  processWriteQueue();
}

function getAudioSamplingRate(rate: number): audio.AudioSamplingRate {
  if (rate === 8000) return audio.AudioSamplingRate.SAMPLE_RATE_8000;
  if (rate === 11025) return audio.AudioSamplingRate.SAMPLE_RATE_11025;
  if (rate === 12000) return audio.AudioSamplingRate.SAMPLE_RATE_12000;
  if (rate === 16000) return audio.AudioSamplingRate.SAMPLE_RATE_16000;
  if (rate === 22050) return audio.AudioSamplingRate.SAMPLE_RATE_22050;
  if (rate === 24000) return audio.AudioSamplingRate.SAMPLE_RATE_24000;
  if (rate === 32000) return audio.AudioSamplingRate.SAMPLE_RATE_32000;
  if (rate === 44100) return audio.AudioSamplingRate.SAMPLE_RATE_44100;
  if (rate === 48000) return audio.AudioSamplingRate.SAMPLE_RATE_48000;
  console.warn('OHOS_Native_ai_engine: TtsWorker unsupported rate ' + rate + ', default 16000');
  return audio.AudioSamplingRate.SAMPLE_RATE_16000;
}

// 自适应缓冲辅助函数
function getMinStartBytes(): number {
  return Math.max(1764, Math.round(ttsSampleRate * 2 * MIN_START_MS / 1000)); // 200ms PCM16
}
function getMaxBufBytes(): number {
  return Math.round(ttsSampleRate * 2 * MAX_BUF_MS / 1000); // 5s PCM16
}
// 检查是否满足起播条件：缓冲够阈值 OR done 已到
function tryFlush(): void {
  if (!rendererReady || !pendingBuffer || pendingBuffer.length === 0) return;
  if (totalChunkBytes < getMinStartBytes() && !pendingDone) return;
  // 满足条件，flush pendingBuffer
  const buf = pendingBuffer;
  pendingBuffer = null;
  const savedBytes = totalChunkBytes;
  totalChunkBytes = 0;
  console.info('OHOS_ai_TTS_FLUSH: chunks=' + buf.length + ' bytes=' + savedBytes + ' minStart=' + getMinStartBytes() + ' done=' + pendingDone);

  // 合并 + 峰值归一化 + 淡入
  let totalBytes: number = 0;
  for (let i = 0; i < buf.length; i++) totalBytes += buf[i].byteLength;
  const merged: ArrayBuffer = new ArrayBuffer(totalBytes);
  const mergedView: Int16Array = new Int16Array(merged);
  let off: number = 0;
  let peak: number = 0;
  for (let i = 0; i < buf.length; i++) {
    const src: Int16Array = new Int16Array(buf[i]);
    for (let j = 0; j < src.length; j++) {
      mergedView[off + j] = src[j];
      const abs: number = Math.abs(src[j]);
      if (abs > peak) peak = abs;
    }
    off += src.length;
  }
  // RMS 归一化（一致性音量，避免句子间忽大忽小）
  // 用全局 TARGET_RMS 替代逐句峰值归一化
  if (peak > 0) {
    // 计算当前 RMS
    let sumSq: number = 0;
    for (let i = 0; i < mergedView.length; i++) {
      sumSq += mergedView[i] * mergedView[i];
    }
    const rms: number = Math.sqrt(sumSq / mergedView.length);
    if (rms > 0) {
      const gain: number = TARGET_RMS * 32768 / rms;
      if (gain > 0.5 && gain < 5.0) {  // 限制增益范围，防止极端放大/缩小
        for (let i = 0; i < mergedView.length; i++) {
          let v: number = Math.round(mergedView[i] * gain);
          mergedView[i] = Math.max(-32768, Math.min(32767, v));
        }
      }
    }
  }
  // 首帧淡入（10ms 线性斜坡，防爆音）
  if (isFirstPlay) {
    isFirstPlay = false;
    const fadeSamples: number = Math.min(Math.round(ttsSampleRate * 0.01), mergedView.length); // 10ms
    for (let i = 0; i < fadeSamples; i++) {
      mergedView[i] = Math.round(mergedView[i] * i / fadeSamples);
    }
  }
  // 入队列播放
  doWrite(merged);
  if (pendingDone) checkAllDone();
}

// 全局 RMS 目标电平（0.3 = -10dB，consistent across sentences）
const TARGET_RMS: number = 0.3;

// 检查所有 TTS 是否全部完成
function checkAllDone(): void {
  if (ttsPendingCount > 0 && ttsCompletedCount >= ttsPendingCount && generationQueue.length === 0) {
    pendingDone = true;
    // 等待写队列清空（确保所有音频已提交给 AudioRenderer）
    if (writeCount > 0 || writeQueueRunning) return;
    console.info('OHOS_ai_TTS: all ' + ttsPendingCount + ' sentences done');
    // 不清理 AudioRenderer — 让它自然播完缓冲
    // 计数器在下一批 startTts 到达时会自动重置
    pendingDone = false;
    ttsPendingCount = 0;
    ttsCompletedCount = 0;
    workerPort.postMessage({ msgType: 'stopped' });
  }
}

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

workerPort.onmessage = async (e: MessageEvents) => {
  const msgType: string = e.data['msgType'] as string;

  if (msgType === 'startTts') {
    const text: string = e.data['text'] as string;
    const modelDir: string = e.data['modelDir'] as string;
    const speed: number = e.data['speed'] as number;
    console.info('OHOS_ai_TTS: startTts textLen=' + text.length + ' pending=' + ttsPendingCount);

    ttsPendingCount++;
    isStopping = false;
    generationQueue.push({ text, modelDir, speed });
    // 启动/继续队列处理（如果已在运行，push 后 while 循环会取到新任务）
    processGenerationQueue();
    return;
  }

  if (msgType === 'stopTts') {
    console.info('OHOS_ai_TTS: stopTts');
    isStopping = true; isPlaying = false;
    nativeLib.stopTts();
    // 清空生成队列
    generationQueue.length = 0;
    generationQueueRunning = false;
    doCleanup();
    ttsPendingCount = 0;
    ttsCompletedCount = 0;
    workerPort.postMessage({ msgType: 'stopped' });
  }
  if (msgType === 'pauseTts') {
    if (audioRenderer) { try { audioRenderer.pause(); } catch (_e) {} }
  }
  if (msgType === 'resumeTts') {
    if (audioRenderer) { try { audioRenderer.start(); } catch (_e) {} }
  }
};

// 生成队列处理：逐个 await 生成，保证多句顺序
async function processGenerationQueue(): Promise<void> {
  if (generationQueueRunning) return;
  generationQueueRunning = true;

  while (generationQueue.length > 0 && !isStopping) {
    const task: TtsTask = generationQueue.shift()!;
    await generateOneSentence(task.text, task.modelDir, task.speed);
  }

  generationQueueRunning = false;
  // 队列清空后检查是否全部完成（可能在 generateOneSentence 中已触发，再确认一次）
  checkAllDone();
}

async function generateOneSentence(text: string, modelDir: string, speed: number): Promise<void> {
  // 初始化（首句才执行）
  if (ttsCompletedCount === 0) {
    isPlaying = true;
    writeCount = 0;
    writeQueue.length = 0;
    writeQueueRunning = false;
    if (audioRenderer) {
      try { await audioRenderer.stop(); } catch (_e) {}
      try { await audioRenderer.start(); } catch (_e) {}
    }
    rendererReady = false;
    pendingBuffer = [];
    ttsSampleRate = 0;
    isFirstPlay = true;
  }

  return new Promise<void>((resolve: Function) => {
    try {
      console.info('OHOS_ai_TTS: generate textLen=' + text.length);

      nativeLib.startStreamingTtsWithSpeed(text, modelDir, speed,
        (data: ArrayBuffer, progress: number, sr: number): void => {
          const chunkTime: number = Date.now();
          if (progress >= 2.0) {
            console.info('OHOS_ai_TTS: sentence done ' + (ttsCompletedCount + 1) + '/' + ttsPendingCount);
            ttsCompletedCount++;
            checkAllDone();
            resolve();
            return;
          }
          if (!data || data.byteLength === 0) return;
          if (ttsSampleRate === 0) {
            ttsSampleRate = sr;
            doCreateRenderer(sr);
          }
          if (!rendererReady) {
            if (!pendingBuffer) pendingBuffer = [];
            if (totalChunkBytes > getMaxBufBytes()) {
              const old: ArrayBuffer | undefined = pendingBuffer.shift();
              if (old) totalChunkBytes -= old.byteLength;
            }
            pendingBuffer.push(data);
            totalChunkBytes += data.byteLength;
            tryFlush();
          } else {
            doWrite(data);
          }
        });
    } catch (err) {
      console.error('OHOS_ai_TTS: error=' + String((err as Error).message || err));
      ttsCompletedCount++;
      checkAllDone();
      resolve();
    }
  });
}

function doCleanup(): void {
  if (audioRenderer) { try { audioRenderer.stop(); audioRenderer.release(); } catch (_e) {} audioRenderer = null; }
  rendererReady = false; pendingBuffer = null; pendingDone = false;
  writeCount = 0; writeQueue.length = 0; writeQueueRunning = false;
  ttsSampleRate = 0; totalChunkBytes = 0;
}

async function doCreateRenderer(sampleRate: number): Promise<void> {
  try {
    const audioStreamInfo: audio.AudioStreamInfo = {
      samplingRate: getAudioSamplingRate(sampleRate),
      channels: audio.AudioChannel.CHANNEL_1,
      sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
      encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW
    };
    const opts: audio.AudioRendererOptions = {
      streamInfo: audioStreamInfo,
      rendererInfo: { content: audio.ContentType.CONTENT_TYPE_SPEECH, usage: audio.StreamUsage.STREAM_USAGE_MEDIA, rendererFlags: 0 }
    };
    audioRenderer = await audio.createAudioRenderer(opts);
    await audioRenderer.start();
    ttsBufferSize = await audioRenderer.getBufferSize();
    rendererReady = true;
    console.info('OHOS_ai_TTS_RENDERER: ready, sr=' + sampleRate + ' bufSize=' + ttsBufferSize);

    // 尝试 flush 缓冲（由 tryFlush 判断是否达到起播阈值）
    tryFlush();
  } catch (e) {
    console.error('OHOS_ai_TTS_RENDERER_FAIL: ' + String(e));
  }
}
