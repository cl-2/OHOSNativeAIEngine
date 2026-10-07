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
let pendingNativeGeneration: number = 0;
let pendingDone: boolean = false;
let completionDrainRunning: boolean = false;
let writeCount: number = 0;
let ttsSampleRate: number = 0;
let totalChunkBytes: number = 0;
let isFirstPlay: boolean = true; // 首帧播放标记，用于淡入
let ttsPendingCount: number = 0;  // 待处理的 TTS 句数
let ttsCompletedCount: number = 0; // 已完成的 TTS 句数
let ttsGenerationId: number = 0;   // stopTts 时 +1，旧轮次回调自动丢弃
let activeGenerationResolve: Function | null = null;
let cleanupPromise: Promise<void> | null = null;

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
const TTS_PCM_GAIN: number = 2.0;     // 对整轮 PCM 使用统一的 2 倍增益

// 写队列：保证串行写入，防电流音
interface PcmQueueItem {
  data: ArrayBuffer;
  nativeGeneration: number;
}
const writeQueue: PcmQueueItem[] = [];
let writeQueueRunning: boolean = false;
let writeQueueBytes: number = 0;
const GENERATION_HIGH_WATER_MS: number = 2500;

// Offline VITS only calls back after a whole natural sentence is inferred.  A
// small phrase pipeline therefore lets the first phrase start immediately,
// while the next phrase is generated during playback of the previous one.
const TTS_PIPELINE_CHARS: number = 14;

function splitForSpeechPipeline(text: string): string[] {
  const source: string = text.trim();
  if (source.length <= TTS_PIPELINE_CHARS) return source.length > 0 ? [source] : [];

  const result: string[] = [];
  let begin: number = 0;
  let preferredEnd: number = -1;
  for (let index: number = 0; index < source.length; index++) {
    const ch: string = source.charAt(index);
    const length: number = index - begin + 1;
    if (ch === '，' || ch === '、' || ch === ',' || ch === '。' || ch === '！' ||
        ch === '？' || ch === '；' || ch === '!' || ch === '?' || ch === ';' || ch === '\n') {
      preferredEnd = index + 1;
    }
    if (length < TTS_PIPELINE_CHARS) continue;
    // Ignore very early commas: their short audio cannot cover the following
    // inference, which was the source of the obvious comma-by-comma pauses.
    let end: number = preferredEnd > begin + 7 ? preferredEnd : index + 1;
    // Do not split an embedded Latin word such as "man" in the middle. This
    // only permits a longer phrase for that word; ordinary Chinese text still
    // keeps the short first-phrase target.
    while (end < source.length && end < begin + TTS_PIPELINE_CHARS + 12) {
      const next: string = source.charAt(end);
      const previous: string = source.charAt(end - 1);
      const nextIsWord: boolean = (next >= 'a' && next <= 'z') ||
        (next >= 'A' && next <= 'Z') || (next >= '0' && next <= '9');
      const previousIsWord: boolean = (previous >= 'a' && previous <= 'z') ||
        (previous >= 'A' && previous <= 'Z') || (previous >= '0' && previous <= '9');
      if (nextIsWord && previousIsWord) {
        end++;
        continue;
      }
      if ((next === '，' || next === ',' || next === '。' || next === '！' || next === '？') &&
          previousIsWord) end++;
      break;
    }
    const part: string = source.substring(begin, end).trim();
    if (part.length > 0) result.push(part);
    begin = end;
    index = end - 1;
    preferredEnd = -1;
  }
  const tail: string = source.substring(begin).trim();
  if (tail.length > 0) result.push(tail);
  return result;
}

function getWriteQueueMs(): number {
  if (ttsSampleRate <= 0) return 0;
  return writeQueueBytes * 1000 / (ttsSampleRate * 2);
}

async function waitForPlaybackCapacity(generation: number): Promise<void> {
  while (!isStopping && generation === ttsGenerationId &&
         getWriteQueueMs() > GENERATION_HIGH_WATER_MS) {
    nativeLib.recordMetric(17, getWriteQueueMs());
    await new Promise<void>((resolve: Function): void => {
      setTimeout((): void => resolve(), 25);
    });
  }
}

async function processWriteQueue(): Promise<void> {
  if (writeQueueRunning) return;
  writeQueueRunning = true;
  while (writeQueue.length > 0 && !isStopping) {
    const item: PcmQueueItem = writeQueue.shift()!;
    await doWriteInternal(item.data);
    nativeLib.ackTtsChunk(item.data.byteLength / 2, item.nativeGeneration);
    writeQueueBytes = Math.max(0, writeQueueBytes - item.data.byteLength);
    nativeLib.recordMetric(17, getWriteQueueMs());
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
      const pcm: Int16Array = new Int16Array(chunk);
      for (let i = 0; i < pcm.length; i++) {
        const amplified: number = Math.round(pcm[i] * TTS_PCM_GAIN);
        pcm[i] = Math.max(-32768, Math.min(32767, amplified));
      }
      await audioRenderer.write(chunk);
    } catch (_e) {
      console.error('OHOS_ai_TTS_WRITE_ERR: ' + String(_e));
    }
    offset += chunkSize;
  }
}

function doWrite(data: ArrayBuffer, nativeGeneration: number): void {
  writeCount++;
  writeQueueBytes += data.byteLength;
  writeQueue.push({ data, nativeGeneration });
  nativeLib.recordMetric(17, getWriteQueueMs());
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

  // 合并后保持模型原始电平。不能只归一化首批缓存，否则后续流式块
  // 回到原始电平时会产生明显的音量断层。
  let totalBytes: number = 0;
  for (let i = 0; i < buf.length; i++) totalBytes += buf[i].byteLength;
  const merged: ArrayBuffer = new ArrayBuffer(totalBytes);
  const mergedView: Int16Array = new Int16Array(merged);
  let off: number = 0;
  for (let i = 0; i < buf.length; i++) {
    const src: Int16Array = new Int16Array(buf[i]);
    for (let j = 0; j < src.length; j++) {
      mergedView[off + j] = src[j];
    }
    off += src.length;
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
  doWrite(merged, pendingNativeGeneration);
  if (pendingDone) checkAllDone();
}

// 检查所有 TTS 是否全部完成
function checkAllDone(): void {
  if (ttsPendingCount > 0 && ttsCompletedCount >= ttsPendingCount && generationQueue.length === 0) {
    pendingDone = true;
    // 等待写队列清空（确保所有音频已提交给 AudioRenderer）
    if (writeCount > 0 || writeQueueRunning) return;
    if (!completionDrainRunning) finishNaturalPlayback();
  }
}

async function finishNaturalPlayback(): Promise<void> {
  completionDrainRunning = true;
  const generationSnapshot: number = ttsGenerationId;
  try {
    if (audioRenderer) await audioRenderer.drain();
  } catch (err) {
    console.warn('OHOS_ai_TTS: renderer drain failed: ' + String(err));
  }
  completionDrainRunning = false;
  if (generationSnapshot !== ttsGenerationId || isStopping) return;
  if (writeCount > 0 || writeQueueRunning || generationQueue.length > 0 ||
      ttsCompletedCount < ttsPendingCount) {
    checkAllDone();
    return;
  }
  console.info('OHOS_ai_TTS: all ' + ttsPendingCount + ' sentences played');
  pendingDone = false;
  isPlaying = false;
  ttsPendingCount = 0;
  ttsCompletedCount = 0;
  workerPort.postMessage({ msgType: 'stopped', reason: 'completed' });
}

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

workerPort.onmessage = async (e: MessageEvents) => {
  const msgType: string = e.data['msgType'] as string;

  if (msgType === 'startTts') {
    if (cleanupPromise) await cleanupPromise;
    const text: string = e.data['text'] as string;
    const modelDir: string = e.data['modelDir'] as string;
    const speed: number = e.data['speed'] as number;
    console.info('OHOS_ai_TTS: startTts textLen=' + text.length + ' pending=' + ttsPendingCount);

    const tasks: string[] = splitForSpeechPipeline(text);
    if (tasks.length === 0) {
      workerPort.postMessage({ msgType: 'stopped', reason: 'completed' });
      return;
    }
    ttsPendingCount += tasks.length;
    isStopping = false;
    for (let index: number = 0; index < tasks.length; index++) {
      generationQueue.push({ text: tasks[index], modelDir, speed });
    }
    console.info('OHOS_ai_TTS: pipeline segments=' + tasks.length + ' pending=' + ttsPendingCount);
    // 启动/继续队列处理（如果已在运行，push 后 while 循环会取到新任务）
    processGenerationQueue();
    return;
  }

  if (msgType === 'stopTts') {
    console.info('OHOS_ai_TTS: stopTts');
    isStopping = true; isPlaying = false;
    ttsGenerationId++;  // 使旧轮次的回调失效
    nativeLib.stopTts();
    if (activeGenerationResolve) {
      activeGenerationResolve();
      activeGenerationResolve = null;
    }
    // 清空生成队列
    generationQueue.length = 0;
    generationQueueRunning = false;
    const currentCleanup: Promise<void> = doCleanup();
    cleanupPromise = currentCleanup;
    await currentCleanup;
    if (cleanupPromise === currentCleanup) cleanupPromise = null;
    completionDrainRunning = false;
    ttsPendingCount = 0;
    ttsCompletedCount = 0;
    workerPort.postMessage({ msgType: 'stopped', reason: 'interrupted' });
  }
  if (msgType === 'releaseTtsRenderer') {
    // The renderer owns buffers outside the native model. Releasing it only
    // after natural completion/stop avoids the historical silent second turn.
    if (!isPlaying && !generationQueueRunning && !completionDrainRunning &&
        writeQueue.length === 0 && !writeQueueRunning) {
      const currentCleanup: Promise<void> = doCleanup();
      cleanupPromise = currentCleanup;
      await currentCleanup;
      if (cleanupPromise === currentCleanup) cleanupPromise = null;
      console.info('OHOS_ai_TTS: idle renderer released');
      workerPort.postMessage({ msgType: 'rendererReleased' });
    }
    return;
  }
  // Candidate VAD must not rewrite queued PCM gain. A confirmed barge-in stops
  // the renderer directly; a rejected candidate leaves playback untouched.
  if (msgType === 'duckTts') return;
  if (msgType === 'resumeTts') return;
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
  const loopGeneration: number = ttsGenerationId;
  generationQueueRunning = true;

  while (generationQueue.length > 0 && !isStopping && loopGeneration === ttsGenerationId) {
    // 当生成速度明显快于播放速度时，在句子边界等待，避免 PCM 队列无限增长。
    await waitForPlaybackCapacity(loopGeneration);
    if (isStopping || loopGeneration !== ttsGenerationId) break;
    const task: TtsTask = generationQueue.shift()!;
    await generateOneSentence(task.text, task.modelDir, task.speed);
  }

  if (loopGeneration !== ttsGenerationId) return;
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
    writeQueueBytes = 0;
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

  // 保存 generation 快照，回调中丢弃旧轮次
  const genSnapshot: number = ttsGenerationId;

  return new Promise<void>((resolve: Function) => {
    activeGenerationResolve = resolve;
    try {
      console.info('OHOS_ai_TTS: generate textLen=' + text.length);

      nativeLib.startStreamingTtsWithSpeed(text, modelDir, speed,
        (data: ArrayBuffer, progress: number, sr: number, nativeGeneration: number): void => {
          if (genSnapshot !== ttsGenerationId) return;  // 旧轮次，丢弃
          const chunkTime: number = Date.now();
          if (progress >= 2.0) {
            console.info('OHOS_ai_TTS: sentence done ' + (ttsCompletedCount + 1) + '/' + ttsPendingCount);
            ttsCompletedCount++;
            checkAllDone();
            activeGenerationResolve = null;
            resolve();
            return;
          }
          if (!data || data.byteLength === 0) return;
          pendingNativeGeneration = nativeGeneration;
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
            doWrite(data, nativeGeneration);
          }
        });
    } catch (err) {
      console.error('OHOS_ai_TTS: error=' + String((err as Error).message || err));
      ttsCompletedCount++;
      checkAllDone();
      activeGenerationResolve = null;
      resolve();
    }
  });
}

async function doCleanup(): Promise<void> {
  if (audioRenderer) {
    try { await audioRenderer.stop(); } catch (_e) {}
    try { await audioRenderer.release(); } catch (_e) {}
    audioRenderer = null;
  }
  rendererReady = false; pendingBuffer = null; pendingDone = false;
  writeCount = 0; writeQueue.length = 0; writeQueueRunning = false; writeQueueBytes = 0;
  ttsSampleRate = 0; totalChunkBytes = 0; pendingNativeGeneration = 0;
}

async function doCreateRenderer(sampleRate: number): Promise<void> {
  try {
    // VOICE_COMMUNICATION can default to the earpiece/call route on phones.
    // Keep the communication stream for platform AEC, but explicitly route
    // this application's communication output to the loudspeaker.
    try {
      const audioManager: audio.AudioManager = audio.getAudioManager();
      const routingManager: audio.AudioRoutingManager = audioManager.getRoutingManager();
      await routingManager.setCommunicationDevice(audio.CommunicationDeviceType.SPEAKER, true);
      const speakerActive: boolean =
        routingManager.isCommunicationDeviceActiveSync(audio.CommunicationDeviceType.SPEAKER);
      console.info('OHOS_ai_TTS_ROUTE: speaker active=' + speakerActive);
    } catch (routeErr) {
      console.warn('OHOS_ai_TTS_ROUTE: force speaker failed: ' + String(routeErr));
    }

    const audioStreamInfo: audio.AudioStreamInfo = {
      samplingRate: getAudioSamplingRate(sampleRate),
      channels: audio.AudioChannel.CHANNEL_1,
      sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
      encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW
    };
    const opts: audio.AudioRendererOptions = {
      streamInfo: audioStreamInfo,
      rendererInfo: {
        content: audio.ContentType.CONTENT_TYPE_SPEECH,
        usage: audio.StreamUsage.STREAM_USAGE_VOICE_COMMUNICATION,
        rendererFlags: 0
      }
    };
    audioRenderer = await audio.createAudioRenderer(opts);
    await audioRenderer.setVolume(1.0);
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
