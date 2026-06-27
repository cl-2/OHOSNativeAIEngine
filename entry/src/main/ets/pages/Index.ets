/*
 * Index.ets – AI 智能对话助手（千问风格 UI）
 * 左侧边栏 + 顶部导航 + 气泡对话 + 底部输入区
 */
import worker, { MessageEvents } from '@ohos.worker';
import { abilityAccessCtrl, Permissions } from '@kit.AbilityKit';
import { fileIo } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import nativeLib from 'libnative_lib.so';
import { getAbilityContext as getAbCtx } from '../utils/ContextHolder';
import MessageBubble from './MessageBubble.ets';
import { MetricsCollector } from '../utils/MetricsCollector';
import { MetricTypeId } from '../utils/MetricsCollector';
import { MemoryManager } from '../utils/MemoryManager';
import { DocumentManager } from '../utils/DocumentManager';
import { BenchmarkRunner } from '../utils/BenchmarkRunner';
import { CryptoManager } from '../utils/CryptoManager';
import { sanitizeLog } from '../utils/LogSanitizer';
import { DevMetricsPanel } from './DevMetricsPanel.ets';
import { util } from '@kit.ArkTS';

// ===== 数据类型 =====
interface ChatItem {
  msg: string;
  from: 'user' | 'assistant';
}

interface HistorySession {
  id: string;
  title: string;
  time: string;
}

interface StopMsgType {
  msgType: string;
}

interface TtsMessage {
  msgType: string;
  text: string;
  modelDir: string;
  speed: number;
}

interface AsrMsg {
  msgType: string;
  asrModelDir: string;
  vadModelPath: string;
  kwsModelPath: string;
}

// ===== 阴影样式接口（匹配 ShadowOptions） =====
interface ShadowConfig {
  radius: number;
  offsetX: number;
  offsetY: number;
  color: string;
}

// ===== 主题常量 =====
const PRIMARY_COLOR: string = '#6366F1';
const PRIMARY_DARK: string = '#4F46E5';
const PRIMARY_LIGHT: string = '#EEF2FF';
const BG_COLOR: string = '#F8F9FC';
const SIDEBAR_COLOR: string = '#FFFFFF';
const NAV_HEIGHT: number = 60;
const BORDER_RADIUS: number = 14;
const SHADOW_STYLE: ShadowConfig = { radius: 10, offsetX: 0, offsetY: 3, color: 'rgba(0,0,0,0.08)' };

// ===== LLM 相关类型和配置 =====
interface LlmMessage {
  role: string;
  content: string;
}

interface LlmChoice {
  message: LlmMessage;
}

interface LlmResponse {
  choices: LlmChoice[];
}

interface ModelOption {
  label: string;
  modelName: string;
  apiUrl: string;
  apiKey: string;
}

const MODEL_LIST: ModelOption[] = [
  { label: 'Qwen2.5 (本地)', modelName: 'qwen2.5:7b', apiUrl: 'http://10.0.2.2:11434/v1/chat/completions', apiKey: '' },
  { label: 'DeepSeek', modelName: 'deepseek-chat', apiUrl: 'https://api.deepseek.com/v1/chat/completions', apiKey: '' },
  { label: 'OpenAI GPT-4o', modelName: 'gpt-4o', apiUrl: 'https://api.openai.com/v1/chat/completions', apiKey: '' },
  { label: '通义千问', modelName: 'qwen-plus', apiUrl: 'https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions', apiKey: '' },
  { label: '智谱 GLM-4', modelName: 'glm-4', apiUrl: 'https://open.bigmodel.cn/api/paas/v4/chat/completions', apiKey: '' },
];

@Entry
@Component
struct Index {
  // ---------- 状态 ----------
  @State chatList: Array<ChatItem> = [];
  @State isRecording: boolean = false;
  @State speed: number = 1.0;
  @State logInfo: string = '';
  @State inputText: string = '';
  @State isSidebarVisible: boolean = false;
  @State activeHistoryId: string = '';
  private currentConversationId: string = ''; // 当前对话的唯一 ID，同一对话只存一个文件
  @State modelStatus: string = '';
  @State modelsReady: boolean = false;

  // 全双工模式状态
  @State fullDuplexActive: boolean = false;
  @State fullDuplexState: string = 'IDLE';
  @State uiRefreshKey: number = 0; // 强制刷新 UI 用
  private _appStartTime: number = Date.now();  // App 启动时间戳，用于计时日志
  private duplexTimer: number = -1;
  private ttsPendingCount: number = 0;       // 流式 TTS 待播句数
  private asrPreWarmWorker?: worker.ThreadWorker; // 提前创建的 ASR Worker（预热用）
  @State useLocalLlm: boolean = false;        // true=本地模型, false=远端 API

  // 开发者性能面板
  @State showDevPanel: boolean = false;
  @State devPanelExpanded: boolean = false;
  @State devBallX: number = 16;
  @State devBallY: number = 200;
  @State devLatestRtf: number = 0;
  @State devRtfColor: string = '#10B981';
  private devBallDragX: number = 0;
  private devBallDragY: number = 0;
  private devPollTimer: number = -1;

  // 全双工模式切换
  private toggleFullDuplex(): void {
    if (this.fullDuplexActive) {
      console.info('OHOS_FLOW: toggleFullDuplex OFF');
      nativeLib.stopFullDuplex();
      this.fullDuplexActive = false;
      this.fullDuplexState = 'IDLE';
      this.logInfo = '全双工已关闭';
      clearInterval(this.duplexTimer);
      // 停止持续录音
      if (this.isRecording || this.asrWorkerInstance) {
        this.stopAsrInternal();
        this.isRecording = false;
      }
    } else {
      console.info('OHOS_FLOW: toggleFullDuplex ON');
      this.lastFinalText = '';
      try {
        nativeLib.startFullDuplex();
        this.fullDuplexActive = true;
        this.logInfo = '全双工模式已开启，自动开始录音…';
        this.duplexTimer = setInterval((): void => {
          this.fullDuplexState = nativeLib.getFullDuplexState() as string;
        }, 500);
        setTimeout((): void => this.toggleRecord(), 200);
      } catch (e) {
        this.logInfo = '全双工启动失败: ' + String(e);
      }
    }
  }

  // 从 EntryAbility 保存的 AbilityContext（用于 C++ rawfile API）
  private getAbilityContext(): Object | undefined {
    return getAbCtx() as Object | undefined;
  }

  // 模拟历史对话数据（启动时从本地加载）
  @State historyList: HistorySession[] = [];

  // ---------- 内部引用 ----------
  private workerInstance?: worker.ThreadWorker;
  private asrWorkerInstance?: worker.ThreadWorker;
  private scrollRef: Scroller = new Scroller();

  // ---------- 生命周期 ----------
  aboutToAppear(): void {
    // 初始化 TTS worker
    this.workerInstance = new worker.ThreadWorker(
      'entry/ets/workers/TtsWorker.ets', { name: 'TTS Worker' }
    );
    this.workerInstance.onmessage = (e: MessageEvents) => {
      const type = e.data['msgType'] as string;
      if (type === 'stopped') {
        this.logInfo = 'TTS stopped';
        if (this.fullDuplexActive) {
          nativeLib.notifyDuplexEvent('tts_complete', '');
        }
      }
    };

    // rawfile 在 HAP 内部，C++ fopen 无法直接读取，需要复制到沙箱
    this.modelStatus = '正在加载模型…(首次约10秒)';
    // 初始化性能监控
    MetricsCollector.init(getContext().filesDir);
    MemoryManager.init(getContext().filesDir);
    DocumentManager.init(getContext().filesDir);
    BenchmarkRunner.init(getContext().filesDir);
    CryptoManager.init();
    MetricsCollector.recordNewConversation();
    this.devPollTimer = setInterval((): void => {
      if (this.showDevPanel && !this.devPanelExpanded) {
        try {
          const rtf: number = MetricsCollector.getLatest(MetricTypeId.AsrRtf);
          this.devLatestRtf = rtf;
          this.devRtfColor = rtf < 0.2 ? '#10B981' : rtf < 0.5 ? '#F59E0B' : '#EF4444';
        } catch (_) {}
      }
    }, 800);
    this.ensureAsrModels();
    this.loadHistoryList();
    // 后台预热 TTS 引擎（用空文本触发预热模式）
    try {
      nativeLib.startStreamingTtsWithSpeed('', this.prepareModelDir(), 1.0, () => {});
    } catch (_) {}
    // 提前创建 AsrWorker 并预热 ASR 引擎（在 Worker 线程内加载，主线程不受影响）
    try {
      const warmWorker: worker.ThreadWorker = new worker.ThreadWorker(
        'entry/ets/workers/AsrWorker.ets', { name: 'ASR Prewarm' }
      );
      warmWorker.postMessage({
        msgType: 'prewarmAsr',
        asrModelDir: this.prepareAsrModelDir(),
        vadModelPath: this.prepareVadModelPath()
      });
      // Worker 预热完成后自我销毁，后续 startAsr 会重建新 Worker（引擎已预热，重建不耗时间）
      // 但更好的做法：保持 Worker 存活，用同一个 Worker 实例
      // 暂存供后续 startAsr 复用
      this.asrPreWarmWorker = warmWorker;
    } catch (_e) {
      console.warn('OHOS_Native_ai_engine: ASR prewarm worker failed: ' + String(_e));
    }
    // 注册全双工 Action 回调（状态机 → ArkTS）
    try {
      nativeLib.setDuplexCallbacks((action: string, payload: string): void => {
        console.info('OHOS_FLOW: duplex action=' + action + ' payload="' + payload.substring(0, 50) + '"');
        if (action === 'llm_request') {
          // 状态机要求调用 LLM → 启动流式 LLM
          this.onLlmRequested(payload);
        } else if (action === 'asr_interim') {
          this.logInfo = '🎤 识别中: ' + payload;
        } else if (action === 'stop_tts') {
          const stopMsg: StopMsgType = { msgType: 'stopTts' };
          this.workerInstance?.postMessage(stopMsg);
        } else if (action === 'interrupted') {
          this.lastFinalText = '';
          this.logInfo = '🔊 检测到打断';
        }
      });
      console.info('OHOS_FLOW: duplex callbacks registered');
    } catch (_e) {
      console.warn('OHOS_FLOW: setDuplexCallbacks failed: ' + String(_e));
    }
    // 网络检测：无网络时自动切本地 LLM 模式
    try {
      import('@ohos.net.connection').then((netConn: Object) => {
        const conn: Record<string, Function> = netConn as Record<string, Function>;
        if (conn.getDefaultNet) {
          conn.getDefaultNet((err: Object, netHandle: Object) => {
            if (err || !netHandle) {
              this.useLocalLlm = true;
              console.info('OHOS_NET: no network, switched to local LLM');
            } else {
              console.info('OHOS_NET: network available');
            }
          });
        }
      }).catch(() => {});
    } catch (_e) {
      console.warn('OHOS_NET: detection failed: ' + String(_e));
    }
  }

  aboutToDisappear(): void {
    this.saveCurrentConversation();
    if (this.devPollTimer >= 0) {
      clearInterval(this.devPollTimer);
      this.devPollTimer = -1;
    }
  }

  // ---------- 复制单个模型文件 ----------
  private async copyOneModel(rawPath: string, destPath: string): Promise<boolean> {
    // 方式 A: C++ RawFile API 读取 + ArkTS fileIo 写入
    try {
      const arrBuf: ArrayBuffer | undefined = nativeLib.readRawFileSync(rawPath);
      if (arrBuf !== undefined) {
        // 创建父目录并删除旧文件
        const destDir: string = destPath.substring(0, destPath.lastIndexOf('/'));
        fileIo.mkdirSync(destDir, true);
        try { fileIo.accessSync(destPath); fileIo.unlinkSync(destPath); } catch (_e) {}
        const file = fileIo.openSync(destPath, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
        const written: number = fileIo.writeSync(file.fd, arrBuf);
        fileIo.closeSync(file);
        console.info('OHOS_Native_ai_engine: C+++ArkTS wrote ' + written + ' bytes: ' + rawPath);
        return true;
      }
    } catch (_a) {
      console.error('OHOS_Native_ai_engine: C+++ArkTS failed: ' + String(_a));
    }

    // 方式 B: ArkTS getRawFd/getRawFileContent (备用)
    try { fileIo.accessSync(destPath); fileIo.unlinkSync(destPath); } catch (_e1) {}
    const ctx = getContext();
    try {
      const rawFdInfo = await ctx.resourceManager.getRawFd(rawPath);
      const destFile = fileIo.openSync(destPath, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
      const readBuf: ArrayBuffer = new ArrayBuffer(rawFdInfo.length);
      fileIo.readSync(rawFdInfo.fd, readBuf, { offset: rawFdInfo.offset, length: rawFdInfo.length });
      fileIo.writeSync(destFile.fd, readBuf);
      fileIo.closeSync(destFile);
      console.info('OHOS_Native_ai_engine: ArkTS copied ' + rawPath);
      return true;
    } catch (_e2) {}
    try {
      const data: Uint8Array = await ctx.resourceManager.getRawFileContent(rawPath);
      const file = fileIo.openSync(destPath, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(file.fd, data);
      fileIo.closeSync(file);
      console.info('OHOS_Native_ai_engine: ArkTS fallback copied ' + rawPath);
      return true;
    } catch (_e3) {}
    return false;
  }

  // ---------- 模型资源初始化 ----------
  private async ensureAsrModels(): Promise<void> {
    try {
      const ctx = getContext();
      const sandboxDir: string = ctx.filesDir + '/models';

      // 快速检查：如果 model.onnx 已存在就跳过复制（节省 10+ 秒）
      try {
        fileIo.accessSync(sandboxDir + '/tts/model.onnx');
        fileIo.accessSync(sandboxDir + '/asr/encoder-epoch-99-avg-1.int8.onnx');
        this.modelsReady = true;
        this.modelStatus = '模型已就绪';
        const tElapsed: number = Date.now() - this._appStartTime;
        console.info('OHOS_load: models ready at +' + tElapsed + 'ms (cached)');
        // 后台预热 TTS 引擎
        try {
          nativeLib.startStreamingTtsWithSpeed('', sandboxDir + '/tts', 1.0, () => {});
        } catch (_) {}
        return;
      } catch (_) {
        console.info('OHOS_Native_ai_engine: Models need copy to sandbox');
      }

      const abCtx: Object | undefined = this.getAbilityContext();
      if (abCtx) {
        nativeLib.initRawfileMgmt(abCtx);
      }

      const files: string[][] = [
        ['models/asr/encoder-epoch-99-avg-1.int8.onnx', sandboxDir + '/asr/encoder-epoch-99-avg-1.int8.onnx'],
        ['models/asr/decoder-epoch-99-avg-1.int8.onnx', sandboxDir + '/asr/decoder-epoch-99-avg-1.int8.onnx'],
        ['models/asr/joiner-epoch-99-avg-1.onnx', sandboxDir + '/asr/joiner-epoch-99-avg-1.onnx'],
        ['models/asr/tokens.txt', sandboxDir + '/asr/tokens.txt'],
        // VAD 模型
        ['models/vad/silero_vad.onnx', sandboxDir + '/vad/silero_vad.onnx'],
        // TTS VITS 模型
        ['models/tts/model.onnx', sandboxDir + '/tts/model.onnx'],
        ['models/tts/tokens.txt', sandboxDir + '/tts/tokens.txt'],
        ['models/tts/lexicon.txt', sandboxDir + '/tts/lexicon.txt'],
      ];

      for (let i: number = 0; i < files.length; i++) {
        if (!(await this.copyOneModel(files[i][0], files[i][1]))) {
          throw new Error('Failed to copy: ' + files[i][0]);
        }
      }

      // 复制 Melo TTS dict 目录
      try {
        const dictDir: string = sandboxDir + '/tts/dict';
        fileIo.mkdirSync(dictDir, true);
        const dictFiles: string[] = ['jieba.dict.utf8', 'hmm_model.utf8', 'user.dict.utf8', 'idf.utf8', 'stop_words.utf8', 'char_state_tab.utf8', 'prob_emit.utf8', 'prob_start.utf8', 'prob_trans.utf8'];
        for (let i: number = 0; i < dictFiles.length; i++) {
          const rawPath: string = 'models/tts/dict/' + dictFiles[i];
          const ok: boolean = await this.copyOneModel(rawPath, dictDir + '/' + dictFiles[i]);
          if (!ok) console.warn('OHOS_TTS: dict skip: ' + rawPath);
        }
        console.info('OHOS_TTS: Melo dict ready');
      } catch (e) {
        console.warn('OHOS_TTS: dict copy skip: ' + String(e));
      }

      this.modelsReady = true;
      const tElapsed: number = Date.now() - this._appStartTime;
      this.modelStatus = '模型已就绪';
      console.info('OHOS_load: models ready at +' + tElapsed + 'ms');
      // 后台预热 TTS 引擎
      try {
        nativeLib.startStreamingTtsWithSpeed('', sandboxDir + '/tts', 1.0, () => {});
        console.info('OHOS_load: TTS prewarm started');
      } catch (_e) {
        console.warn('OHOS_load: TTS prewarm failed: ' + String(_e));
      }
    } catch (e) {
      console.error('OHOS_Native_ai_engine: Model error: ' + String(e));
      this.modelStatus = '模型加载失败: ' + String(e);
    }
  }

  // ---------- ASR ----------
  private async startAsr(): Promise<void> {
    console.info('OHOS_FLOW: startAsr called, modelsReady=' + this.modelsReady +
      ' workerExists=' + (this.asrWorkerInstance !== undefined) +
      ' fullDuplex=' + this.fullDuplexActive);
    // 检查模型是否就绪
    if (!this.modelsReady) {
      this.logInfo = '⏳ 模型正在加载，请稍候…';
      return;
    }
    try {
      const at = abilityAccessCtrl.createAtManager();
      const perms: Array<Permissions> = ['ohos.permission.MICROPHONE'];
      const res = await at.requestPermissionsFromUser(getContext(), perms);
      if (res.authResults[0] !== 0) { this.logInfo = '麦克风权限被拒绝'; return; }
    } catch (e) {
      console.error('OHOS_Native_ai_engine: Index startAsr permission error: ' + String(e));
      this.logInfo = '权限申请失败'; return;
    }
    if (!this.asrWorkerInstance) {
      // 优先复用预热好的 Worker
      if (this.asrPreWarmWorker) {
        this.asrWorkerInstance = this.asrPreWarmWorker;
        this.asrPreWarmWorker = undefined;
        this.asrWorkerInstance.onmessage = (e: MessageEvents): void => {
          const type = e.data['msgType'] as string;
          console.info('OHOS_Native_ai_engine: Index received worker msg: ' + type);
          if (type === 'asr-result') {
            this.handleAsrResult(e.data['text'] as string, e.data['isFinal'] as boolean);
          } else if (type === 'asr-stopped') {
            this.isRecording = false;
            const err: string = e.data['error'] as string;
            this.logInfo = err ? '❌ ' + err : '录音已停止';
          }
        };
      } else {
        this.asrWorkerInstance = new worker.ThreadWorker(
          'entry/ets/workers/AsrWorker.ets', { name: 'ASR Worker' }
        );
        this.asrWorkerInstance.onmessage = (e: MessageEvents): void => {
          const type = e.data['msgType'] as string;
          console.info('OHOS_Native_ai_engine: Index received worker msg: ' + type);
          if (type === 'asr-result') {
            this.handleAsrResult(e.data['text'] as string, e.data['isFinal'] as boolean);
          } else if (type === 'asr-stopped') {
            this.isRecording = false;
            const err: string = e.data['error'] as string;
            this.logInfo = err ? '❌ ' + err : '录音已停止';
          }
        };
      }
    }
    const asrMsg: AsrMsg = {
      msgType: 'startAsr',
      asrModelDir: this.prepareAsrModelDir(),
      vadModelPath: this.prepareVadModelPath(),
      kwsModelPath: this.prepareKwsModelPath()
    };
    this.asrWorkerInstance.postMessage(asrMsg);
    this.isRecording = true;
    this.logInfo = '正在录音…';
  }

  private lastFinalText: string = '';

  // 对话历史上下文（用于 LLM 多轮对话）
  private llmHistory: LlmMessage[] = [];
  @State currentModelIdx: number = 0;
  @State isModelPickerVisible: boolean = false;

  // ========== 本地 LLM（Qwen2.5-0.5B + ONNX Runtime）==========

  private callLocalLlmStreaming(
    text: string,
    onToken: (token: string) => void,
    onComplete: (fullText: string) => void
  ): void {
    console.info('OHOS_LLM_LOCAL: calling local LLM, text="' + text.substring(0, 50) + '"');
    let fullText: string = '';
    try {
      nativeLib.callLocalLlm(text, '',
        (token: string): void => {
          fullText += token;
          onToken(token);
        },
        (resultText: string, success: boolean): void => {
          console.info('OHOS_LLM_LOCAL: done, success=' + success + ' len=' + resultText.length);
          onComplete(success ? resultText : '');
        }
      );
    } catch (e) {
      console.error('OHOS_LLM_LOCAL: error=' + String(e));
      onComplete('');
    }
  }

  // ========== 远端 LLM（HTTP API）==========

  private async callLlm(userText: string): Promise<string> {
    const model: ModelOption = MODEL_LIST[this.currentModelIdx];
    const url: string = model.apiUrl;
    const apiKey: string = model.apiKey;
    const modelName: string = model.modelName;

    // 构建含记忆 + RAG 信息的 System Prompt
    const memoryPrompt: string = MemoryManager.getMemoryPrompt();
    const ragContext: string = DocumentManager.buildContext(userText);
    let sysContent: string = '你是一个智能AI助手，请用简洁友好的方式回答用户的问题。';
    if (memoryPrompt) {
      sysContent += '\n\n关于用户的信息：\n' + memoryPrompt;
    }
    if (ragContext) {
      sysContent += '\n\n' + ragContext;
    }
    sysContent += '\n\n请用这些信息来提供更个性化的回答。';
    const sysMsg: LlmMessage = { role: 'system', content: sysContent };
    const userMsg: LlmMessage = { role: 'user', content: userText };
    const allMessages: LlmMessage[] = [sysMsg, ...this.llmHistory, userMsg];

    const requestBody: string = JSON.stringify({
      model: modelName,
      messages: allMessages,
      max_tokens: 512,
      temperature: 0.7,
      stream: false
    });

    const authHeader: string = apiKey ? 'Bearer ' + apiKey : '';
    const requestHeaders: Record<string, string> = {
      'Content-Type': 'application/json',
      'Authorization': authHeader
    };

    try {
      const httpReq: http.HttpRequest = http.createHttp();
      const response: http.HttpResponse = await httpReq.request(
        url,
        {
          method: http.RequestMethod.POST,
          header: requestHeaders,
          extraData: requestBody,
          connectTimeout: 15000,
          readTimeout: 60000
        }
      );
      httpReq.destroy();

      if (response.responseCode === 200) {
        const respStr: string = response.result as string;
        const respObject: object = JSON.parse(respStr);
        const respJson: Record<string, object[]> = respObject as Record<string, object[]>;
        const choices: object[] = respJson['choices'];
        if (choices && choices.length > 0) {
          const choice: Record<string, object | string> = choices[0] as Record<string, object | string>;
          const message: Record<string, string> = choice['message'] as Record<string, string>;
          const reply: string = message['content'] || '';
          if (reply) {
            this.llmHistory.push(userMsg);
            const asstMsg: LlmMessage = { role: 'assistant', content: reply };
            this.llmHistory.push(asstMsg);
            if (this.llmHistory.length > 20) {
              this.llmHistory = this.llmHistory.slice(-20);
            }
            return reply;
          }
        }
        return '抱歉，AI 返回了空响应。';
      } else {
        const errBody: string = JSON.stringify(response.result);
        console.error('OHOS_Native_ai_engine: LLM HTTP error: ' + response.responseCode + ', body=' + errBody);
        return '网络请求失败（' + response.responseCode + '），请检查 LLM 服务地址或 API Key。';
      }
    } catch (err) {
      const errObj: object = err as object;
      const errJson: string = JSON.stringify(errObj);
      console.error('OHOS_Native_ai_engine: LLM call failed: ' + errJson);
      return '调用大模型失败，请检查网络和 API 配置。';
    }
  }

  // LLM 流式调用（旧版 typewriter 方式，保留兼容）
  private callLlmStreaming(
    userText: string,
    onToken: (token: string) => void,
    onComplete: (fullText: string) => void
  ): void {
    // 本地 LLM 模式：走 ONNX Runtime 推理
    if (this.useLocalLlm) {
      this.callLocalLlmStreaming(userText, onToken, onComplete);
      return;
    }

    const model: ModelOption = MODEL_LIST[this.currentModelIdx];
    const url: string = model.apiUrl;
    const apiKey: string = model.apiKey;
    const modelName: string = model.modelName;

    // 构建含记忆 + RAG 信息的 System Prompt
    const memPrompt: string = MemoryManager.getMemoryPrompt();
    const ragCtx: string = DocumentManager.buildContext(userText);
    let sysContent2: string = '你是一个智能AI助手，请用简洁友好的方式回答用户的问题。';
    if (memPrompt) {
      sysContent2 += '\n\n关于用户的信息：\n' + memPrompt;
    }
    if (ragCtx) {
      sysContent2 += '\n\n' + ragCtx;
    }
    sysContent2 += '\n\n请用这些信息来提供更个性化的回答。';
    const sysMsg: LlmMessage = { role: 'system', content: sysContent2 };
    const userMsg: LlmMessage = { role: 'user', content: userText };
    const allMessages: LlmMessage[] = [sysMsg, ...this.llmHistory, userMsg];

    const requestBody: string = JSON.stringify({
      model: modelName,
      messages: allMessages,
      max_tokens: 512,
      temperature: 0.7,
      stream: true
    });

    const authHeader: string = apiKey ? 'Bearer ' + apiKey : '';

    try {
      const httpReq: http.HttpRequest = http.createHttp();
      let fullText: string = '';
      let pendingSentence: string = '';
      let streamDone: boolean = false;

      // ---- UI 线程每 30ms 轮询 buffer ----
      let pendingBatch: string = '';
      const uiTimer: number = setInterval((): void => {
        if (pendingBatch) {
          const batch: string = pendingBatch;
          pendingBatch = '';
          onToken(batch);
        }
        if (streamDone && !pendingBatch) {
          clearInterval(uiTimer);
          onComplete(fullText);
        }
      }, 30);

      // SSE 增量数据
      const textDecoder: util.TextDecoder = util.TextDecoder.create('utf-8', { fatal: false });
      httpReq.on('dataReceive', (data: ArrayBuffer): void => {
        const uint8: Uint8Array = new Uint8Array(data);
        const chunk: string = textDecoder.decodeWithStream(uint8);
        const lines: string[] = chunk.split('\n');
        for (const line of lines) {
          if (line.startsWith('data: ')) {
            const dataStr: string = line.substring(6).trim();
            if (dataStr === '[DONE]') continue;
            try {
              const json: Record<string, object> = JSON.parse(dataStr) as Record<string, object>;
              const choices: object[] = json['choices'] as object[];
              if (choices && choices.length > 0) {
                const choice: Record<string, object> = choices[0] as Record<string, object>;
                const delta: Record<string, string> = choice['delta'] as Record<string, string>;
                const content: string = delta?.['content'] || '';
                if (content) {
                  fullText += content;
                  pendingBatch += content;
                  pendingSentence += content;
                  if (/[。！？\n]/.test(content) && pendingSentence.trim().length >= 2) {
                    pendingSentence = '';
                  }
                }
              }
            } catch (_e) {}
          }
        }
      });

      httpReq.on('dataEnd', (): void => {
        if (streamDone) return;
        streamDone = true;
        httpReq.destroy();
        console.info('OHOS_LLM: dataEnd, fullText len=' + (fullText ? fullText.length : 0));
        // TTS 在 onComplete 中一次性整段生成
        if (fullText) {
          this.llmHistory.push(userMsg);
          const asstMsg: LlmMessage = { role: 'assistant', content: fullText };
          this.llmHistory.push(asstMsg);
          if (this.llmHistory.length > 20) {
            this.llmHistory = this.llmHistory.slice(-20);
          }
        }
      });

      // 使用 requestInStream 获取流式响应
      httpReq.requestInStream(url, {
        method: http.RequestMethod.POST,
        header: {
          'Content-Type': 'application/json',
          'Authorization': authHeader
        },
        extraData: requestBody,
        connectTimeout: 15000,
        readTimeout: 60000
      }).then((code: number): void => {
        if (code !== 200 && !fullText) {
          // 请求失败且无流式数据
          console.error('OHOS_LLM_STREAM_ERR: code=' + code);
          streamDone = true;
          pendingBatch = '调用大模型失败，状态码：' + code;
        }
      }).catch((err: Error): void => {
        console.error('OHOS_LLM_STREAM_FAILED: ' + JSON.stringify(err));
        streamDone = true;
        if (!fullText) {
          pendingBatch = '调用大模型失败，请检查网络和 API 配置。';
        }
      });
    } catch (err) {
      console.error('OHOS_LLM_ERR: ' + JSON.stringify(err));
    }
  }

  private handleAsrResult(text: string, isFinal: boolean): void {
    // 去重：移除连续重复3次以上的字符（模型叠字修复）
    if (isFinal && text.length > 1) {
      let deduped: string = '';
      let prev: string = '';
      let repeatCount: number = 0;
      for (let i: number = 0; i < text.length; i++) {
        const ch: string = text[i];
        if (ch === prev) {
          repeatCount++;
          if (repeatCount >= 2) continue; // 跳过第三个及以上连续相同字符
        } else {
          repeatCount = 0;
        }
        deduped += ch;
        prev = ch;
      }
      if (deduped !== text) {
        console.info('OHOS_FLOW: dedup "' + sanitizeLog(text) + '" -> "' + sanitizeLog(deduped) + '"');
        text = deduped;
      }
    }
    console.info('OHOS_FLOW: handleAsrResult text="' + sanitizeLog(text) + '" isFinal=' + isFinal +
      ' fullDuplex=' + this.fullDuplexActive + ' at +' + (Date.now() - this._appStartTime) + 'ms');
    if (isFinal) {
      if (text === this.lastFinalText) {
        console.info('OHOS_FLOW: dedup final result, skip');
        return;
      }
      this.lastFinalText = text;
      this.isRecording = false;
      // 全双工模式：状态机通过 setDuplexCallbacks → 'llm_request' 触发 LLM 调用
      // 不需要在此处调用 sendTextMsg
      if (!this.fullDuplexActive) {
        this.stopAsrInternal();
        this.inputText = text;
        this.logInfo = '🤖 AI 思考中…';
        this.sendTextMsg();
      } else {
        this.logInfo = '🎤 识别完成，等待 AI 回复…';
      }
    }
  }

  // ---------- TTS ----------
  private startTts(text: string): void {
    console.info('OHOS_ai_STARTTTS: textLen=' + text.length + ' worker=' + (this.workerInstance !== undefined) + ' pending=' + this.ttsPendingCount);
    if (!text) return;

    // 检查 worker 状态，异常时重建
    if (!this.workerInstance) {
      console.info('OHOS_FLOW: TTS worker missing, re-creating');
      try {
        this.workerInstance = new worker.ThreadWorker('entry/ets/workers/TtsWorker.ets', { name: 'TTS Worker' });
      } catch (e) {
        console.error('OHOS_FLOW: failed to recreate TTS worker: ' + String(e));
        return;
      }
    }

    this.ttsPendingCount++;

    try {
      // TTS Worker 内部已按流水线处理多句，完成后发一次 'stopped'
      const ttsMsg: TtsMessage = {
        msgType: 'startTts', text, modelDir: this.prepareModelDir(), speed: this.speed
      };
      this.workerInstance.postMessage(ttsMsg);
      if (this.fullDuplexActive) {
        nativeLib.notifyDuplexEvent('tts_started', '');
      }
    } catch (e) {
      console.error('OHOS_FLOW: startTts error=' + String(e));
      this.ttsPendingCount--;
      // 标记 worker 失效，下次重建
      this.workerInstance = undefined;
    }
  }

  // ---------- UI 交互 ----------
  // 停止 ASR 但不影响 isRecording 状态
  private stopAsrInternal(): void {
    const stopMsg: StopMsgType = { msgType: 'stopAsr' };
    this.asrWorkerInstance?.postMessage(stopMsg);
  }

  private toggleRecord(): void {
    if (this.isRecording) {
      const stopMsg: StopMsgType = { msgType: 'stopAsr' };
      this.asrWorkerInstance?.postMessage(stopMsg);
      this.isRecording = false;
      this.logInfo = '已停止录音';
    } else {
      if (!this.modelsReady) {
        this.logInfo = this.modelStatus ? '⏳ ' + this.modelStatus : '⏳ 模型加载中…';
        return;
      }
      this.inputText = ''; // 清空输入框，准备显示实时识别结果
      this.lastFinalText = ''; // 重置去重状态
      this.logInfo = '🎤 录音中…';
      this.startAsr();
    }
  }

  private clearConversation(): void {
    this.chatList = [];
    this.inputText = '';
    this.scrollToBottom();
  }

  private scrollToBottom(): void {
    setTimeout((): void => this.scrollRef.scrollEdge(Edge.Bottom), 100);
  }

  private sendTextMsg(): void {
    const text: string = this.inputText.trim();
    console.info('OHOS_FLOW: sendTextMsg text="' + sanitizeLog(text.substring(0, 50)) + '" fullDuplex=' + this.fullDuplexActive);
    if (!text) return;
    // 更新会话状态
    if (this.currentConversationId) {
      MemoryManager.touchSession(this.currentConversationId);
    }
    this.saveCurrentConversation();
    const userMsg: ChatItem = { msg: text, from: 'user' };
    this.chatList.push(userMsg);
    MetricsCollector.recordMessage('user');
    this.inputText = '';
    this.scrollToBottom();
    this.logInfo = 'AI 思考中…';
    // 记录当前对话 ID，用于回调时校验是否还是同一个对话
    const reqConvId: string = this.currentConversationId;
    const llmStartTime: number = Date.now();
    // 插入空的 assistant 消息占位，后续 onToken 逐步填充内容
    const aiMsg: ChatItem = { msg: '', from: 'assistant' };
    this.chatList.push(aiMsg);
    this.scrollToBottom();
    const llmReqStart: number = Date.now();
    this.callLlmStreaming(text,
      (token: string): void => {
        if (this.currentConversationId !== reqConvId) return;
        const idx: number = this.chatList.length - 1;
        if (idx >= 0 && this.chatList[idx].from === 'assistant') {
          const updated: ChatItem = { msg: this.chatList[idx].msg + token, from: 'assistant' };
          this.chatList[idx] = updated;
          this.uiRefreshKey++;
        }
        this.scrollToBottom();
      },
      (fullText: string): void => {
        // 记录 LLM 延迟（首 token 时间用 llmStartTime，此处为完整时间）
        const llmLatency: number = Date.now() - llmStartTime;
        MetricsCollector.recordLlmLatency(llmLatency);
        // 如果用户已切换到其他对话，丢弃此回复
        if (this.currentConversationId !== reqConvId) {
          console.info('OHOS_Native_ai_engine: discarding LLM streaming reply (switched conversation)');
          return;
        }
        // 如果最终回复为空（出错），设置占位消息为错误提示
        if (!fullText) {
          const idx: number = this.chatList.length - 1;
          if (idx >= 0 && this.chatList[idx].from === 'assistant' && !this.chatList[idx].msg) {
            this.chatList[idx].msg = '抱歉，AI 返回了空响应。';
            this.chatList = [...this.chatList];
          }
        } else {
          const idx: number = this.chatList.length - 1;
          if (idx >= 0 && this.chatList[idx].from === 'assistant') {
            this.chatList[idx].msg = fullText;
            this.uiRefreshKey++;
          }
        }
        MetricsCollector.recordMessage('assistant');
        // 更新长期记忆
        if (fullText) {
          MemoryManager.extractFactsFromConversation(text, fullText);
          MemoryManager.incrementConversationCount();
        }
        this.logInfo = '';
        this.scrollToBottom();
        this.saveCurrentConversation();
        console.info('OHOS_ai_ONCOMPLETE: textLen=' + (fullText ? fullText.length : 0) + ' duplex=' + this.fullDuplexActive);
        if (this.fullDuplexActive) {
          // 拆分文本：第一句单独送（短→encoder快），余下紧随其后
          // TTS Worker 内部串行化处理，保证顺序
          if (fullText) {
            const firstSentMatch: RegExpMatchArray | null = fullText.match(/^.*?[。！？\n]/);
            if (firstSentMatch && firstSentMatch[0].length < fullText.length * 0.6) {
              this.startTts(firstSentMatch[0]);
              this.startTts(fullText.substring(firstSentMatch[0].length));
            } else {
              this.startTts(fullText);
            }
          }
          nativeLib.notifyDuplexEvent('llm_complete', fullText || '');
        }
      }
    );
  }

  // ========== 状态机 Action 回调 ==========

  // 由 setDuplexCallbacks → 'llm_request' 触发，状态机要求调用 LLM
  private onLlmRequested(text: string): void {
    if (!text || !this.fullDuplexActive) return;
    console.info('OHOS_FLOW: onLlmRequested text="' + sanitizeLog(text.substring(0, 50)) + '"');
    const reqConvId: string = this.currentConversationId;
    const llmStartTime: number = Date.now();

    // 插入用户消息
    const userMsg: ChatItem = { msg: text, from: 'user' };
    this.chatList.push(userMsg);
    this.scrollToBottom();
    // 插入空的 assistant 占位
    const aiMsg: ChatItem = { msg: '', from: 'assistant' };
    this.chatList.push(aiMsg);
    this.scrollToBottom();
    this.logInfo = '🤖 AI 思考中…';

    this.callLlmStreaming(text,
      (token: string): void => {
        if (this.currentConversationId !== reqConvId) return;
        const idx: number = this.chatList.length - 1;
        if (idx >= 0 && this.chatList[idx].from === 'assistant') {
          const updated: ChatItem = { msg: this.chatList[idx].msg + token, from: 'assistant' };
          this.chatList[idx] = updated;
          this.uiRefreshKey++;
        }
        this.scrollToBottom();
      },
      (fullText: string): void => {
        MetricsCollector.recordLlmLatency(Date.now() - llmStartTime);
        if (this.currentConversationId !== reqConvId) return;
        if (!fullText) {
          const idx: number = this.chatList.length - 1;
          if (idx >= 0 && this.chatList[idx].from === 'assistant' && !this.chatList[idx].msg) {
            this.chatList[idx].msg = '抱歉，AI 返回了空响应。';
            this.chatList = [...this.chatList];
          }
        } else {
          const idx: number = this.chatList.length - 1;
          if (idx >= 0 && this.chatList[idx].from === 'assistant') {
            this.chatList[idx].msg = fullText;
            this.uiRefreshKey++;
          }
        }
        MetricsCollector.recordMessage('assistant');
        if (fullText) {
          MemoryManager.extractFactsFromConversation(text, fullText);
          MemoryManager.incrementConversationCount();
        }
        this.logInfo = '';
        this.scrollToBottom();
        this.saveCurrentConversation();
        // 拆分文本：第一句单独送（短→encoder快），余下紧随其后
        if (fullText) {
          const firstSentMatch: RegExpMatchArray | null = fullText.match(/^.*?[。！？\n]/);
          if (firstSentMatch && firstSentMatch[0].length < fullText.length * 0.6) {
            this.startTts(firstSentMatch[0]);
            this.startTts(fullText.substring(firstSentMatch[0].length));
          } else {
            this.startTts(fullText);
          }
        }
        nativeLib.notifyDuplexEvent('llm_complete', fullText || '');
      }
    );
  }

  // ========== 历史记录管理 ==========

  private saveCurrentConversation(): void {
    if (this.chatList.length < 2) return;
    const ctx = getContext();
    const historyDir: string = ctx.filesDir + '/history';
    try { fileIo.mkdirSync(historyDir); } catch (_x) {}
    const now: Date = new Date();
    const timeStr: string = (now.getMonth() + 1) + '/' + now.getDate() + ' ' +
      now.getHours().toString().padStart(2, '0') + ':' + now.getMinutes().toString().padStart(2, '0');
    // 复用当前对话 ID，同一对话只更新同一个文件
    if (!this.currentConversationId) {
      this.currentConversationId = now.getTime().toString();
    }
    const id: string = this.currentConversationId;
    let title: string = '新对话';
    for (let i: number = 0; i < this.chatList.length; i++) {
      if (this.chatList[i].from === 'user') {
        const t: string = this.chatList[i].msg;
        title = t.length > 20 ? t.substring(0, 20) + '…' : t;
        break;
      }
    }
    const session: HistorySession = { id, title, time: timeStr };
    const data: string = JSON.stringify({ session, chatList: this.chatList });
    try {
      const filePath: string = historyDir + '/' + id + '.json';
      const file = fileIo.openSync(filePath, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY);
      fileIo.writeSync(file.fd, data);
      fileIo.closeSync(file);
      console.info('OHOS_Native_ai_engine: history saved, id=' + id);
      this.loadHistoryList();
    } catch (e) {
      console.error('OHOS_Native_ai_engine: save history failed: ' + String(e));
    }
  }

  private loadHistoryList(): void {
    const ctx = getContext();
    const historyDir: string = ctx.filesDir + '/history';
    try { fileIo.accessSync(historyDir); } catch (_x) { return; }
    const sessions: HistorySession[] = [];
    try {
      const files: string[] = fileIo.listFileSync(historyDir);
      for (let i: number = 0; i < files.length; i++) {
        if (files[i].endsWith('.json')) {
          try {
            const content: string = fileIo.readTextSync(historyDir + '/' + files[i]);
            const data: Record<string, Object> = JSON.parse(content) as Record<string, Object>;
            const s: HistorySession = data['session'] as HistorySession;
            if (s && s.id) sessions.push(s);
          } catch (_pe) {}
        }
      }
    } catch (_le) {}
    sessions.sort((a: HistorySession, b: HistorySession): number => parseInt(b.id) - parseInt(a.id));
    this.historyList = sessions;
  }

  private restoreConversation(sessionId: string): void {
    console.info('OHOS_Native_ai_engine: restore id=' + sessionId);
    this.currentConversationId = sessionId;
    this.activeHistoryId = sessionId;
    const ctx = getContext();
    try {
      const content: string = fileIo.readTextSync(ctx.filesDir + '/history/' + sessionId + '.json');
      const data: Record<string, Object> = JSON.parse(content) as Record<string, Object>;
      const items: Record<string, string>[] = data['chatList'] as Record<string, string>[];
      console.info('OHOS_Native_ai_engine: restore got ' + (items ? items.length : 0));
      if (items && items.length > 0) {
        const newList: ChatItem[] = [];
        for (let i: number = 0; i < items.length; i++) {
          newList.push({ msg: items[i]['msg'], from: items[i]['from'] as 'user' | 'assistant' });
        }
        this.chatList = newList;
        this.inputText = '';
        this.llmHistory = [];
        this.lastFinalText = '';
        this.logInfo = '';
        // 延迟一帧滚动到底部
        setTimeout((): void => {
          this.scrollRef.scrollEdge(Edge.Bottom);
        }, 100);
      }
    } catch (_e) {
      console.error('OHOS_Native_ai_engine: restore failed for ' + sessionId);
    }
  }

  private deleteHistory(sessionId: string): void {
    const ctx = getContext();
    try {
      fileIo.unlinkSync(ctx.filesDir + '/history/' + sessionId + '.json');
      this.loadHistoryList();
      if (this.activeHistoryId === sessionId) {
        this.activeHistoryId = '';
        this.currentConversationId = '';
        this.chatList = [];
        this.inputText = '';
        this.llmHistory = [];
        this.lastFinalText = '';
        this.logInfo = '对话已删除';
      }
    } catch (_e) {}
  }

  private clearAllHistory(): void {
    const ctx = getContext();
    try {
      const files: string[] = fileIo.listFileSync(ctx.filesDir + '/history');
      for (let i: number = 0; i < files.length; i++) {
        if (files[i].endsWith('.json')) {
          try { fileIo.unlinkSync(ctx.filesDir + '/history/' + files[i]); } catch (_x) {}
        }
      }
      this.historyList = [];
      this.chatList = [];
      this.currentConversationId = '';
      this.saveCurrentConversation();
    } catch (_e) {}
  }

  // ---------- 文档管理 ----------
  private importDocument(): void {
    // 使用文件选择器导入文档
    // OHOS 上通过 filePicker 或直接粘贴文本
    // 简单实现：弹出对话框输入文本
    AlertDialog.show({
      title: '导入文档',
      message: '输入文档名称和内容',
      autoCancel: true,
      primaryButton: {
        value: '取消',
        action: (): void => {}
      },
      secondaryButton: {
        value: '确认导入',
        action: (): void => {
          // 使用一个默认测试文档
          const sampleDoc: string = '体检报告\n姓名：张三\n日期：2025年12月\n\n血压收缩压：120mmHg\n血压舒张压：80mmHg\n心率：72次/分\n体温：36.5℃\n\n血常规：正常\n尿常规：正常\n肝功能：正常';
          DocumentManager.importDocument('体检报告.txt', sampleDoc);
        }
      }
    });
  }

  private showDocumentList(): void {
    const docs = DocumentManager.getDocuments();
    let msg: string = docs.length === 0 ? '暂无文档' : '';
    for (let i: number = 0; i < docs.length; i++) {
      msg += (i + 1) + '. ' + docs[i].title + ' (' + docs[i].wordCount + '字)\n';
    }
    AlertDialog.show({
      title: '已导入的文档',
      message: msg || '暂无文档',
      confirm: { value: '关闭', action: (): void => {} }
    });
  }

  private newConversation(): void {
    this.saveCurrentConversation();
    this.currentConversationId = '';
    this.chatList = [];
    this.inputText = '';
    this.llmHistory = [];
    this.activeHistoryId = '';
    this.lastFinalText = '';
    this.logInfo = '新对话已创建';
    MetricsCollector.recordNewConversation();
    this.loadHistoryList();
  }

  // ---------- 资源准备（指向沙箱中的模型路径） ----------
  private prepareAsrModelDir(): string {
    return getContext().filesDir + '/models/asr';
  }

  private prepareVadModelPath(): string {
    return getContext().filesDir + '/models/vad/silero_vad.onnx';
  }

  private prepareKwsModelPath(): string {
    return ''; // 暂不使用唤醒词
  }

  private prepareModelDir(): string {
    return getContext().filesDir + '/models/tts';
  }

  // ========== 渲染 ==========
  build(): void {
    Stack() {
      // ===== 主内容区（全宽） =====
      Column() {
        // ---- 顶部导航栏 ----
        this.NavBar()

        // ---- 对话消息区 ----
        Scroll(this.scrollRef) {
          Column() {
            // 欢迎消息
            if (this.chatList.length === 0) {
              this.WelcomeSection()
            }
            // 消息列表
            ForEach(this.chatList, (item: ChatItem, index: number) => {
              MessageBubble({
                msg: item.msg,
                from: item.from,
                speakClick: (): void => {
                  if (item.from === 'assistant' && item.msg.length > 0) {
                    this.startTts(item.msg);
                  }
                }
              })
            }, (item: ChatItem, index: number): string => 'msg_' + index + '_' + this.uiRefreshKey)
            // 底部占位
            Blank().height(16)
          }
          .width('100%')
          .padding({ left: 16, right: 16, top: 8 })
        }
        .layoutWeight(1)
        .backgroundColor(BG_COLOR)

        // ---- 底部输入区 ----
        this.BottomInput()
      }
      .width('100%')
      .height('100%')

      // ===== 侧边栏覆盖层（仅展开时显示） =====
      if (this.isSidebarVisible) {
        Row() {
          // 侧边栏面板
          this.Sidebar()
          // 半透明遮罩 — 点击关闭侧边栏
          Column()
            .width('100%')
            .height('100%')
            .backgroundColor('rgba(0,0,0,0.35)')
            .onClick((): void => { this.isSidebarVisible = false; })
        }
        .width('100%')
        .height('100%')
      }

      // ===== 模型选择器覆盖层 =====
      if (this.isModelPickerVisible) {
        Row() {
          Column()
            .width('100%')
            .height('100%')
            .backgroundColor('rgba(0,0,0,0.40)')
            .onClick((): void => { this.isModelPickerVisible = false; })
          this.ModelPicker()
        }
        .width('100%')
        .height('100%')
      }

      // ===== 开发者模式：悬浮小球（不拦截任何下层交互） =====
      if (this.showDevPanel && !this.devPanelExpanded) {
        Column() {
          Text(this.devLatestRtf.toFixed(2))
            .fontSize(12).fontColor('#FFFFFF').fontWeight('700')
          Text('RTF')
            .fontSize(7).fontColor('rgba(255,255,255,0.7)').margin({ top: -2 })
        }
        .width(48).height(48)
        .backgroundColor(this.devRtfColor)
        .borderRadius(24)
        .alignItems(HorizontalAlign.Center).justifyContent(FlexAlign.Center)
        .shadow({ radius: 8, offsetX: 0, offsetY: 3, color: 'rgba(0,0,0,0.25)' })
        .position({ x: this.devBallX, y: this.devBallY })
        .onClick((): void => { this.devPanelExpanded = true; })
        .gesture(
          PanGesture()
            .onActionStart((): void => {
              this.devBallDragX = this.devBallX;
              this.devBallDragY = this.devBallY;
            })
            .onActionUpdate((e: GestureEvent): void => {
              this.devBallX = this.devBallDragX + e.offsetX;
              this.devBallY = this.devBallDragY + e.offsetY;
            })
        )
      }

      // ===== 开发者模式：展开面板 =====
      if (this.showDevPanel && this.devPanelExpanded) {
        DevMetricsPanel({
          showPanel: $showDevPanel,
          isExpanded: $devPanelExpanded
        })
      }
    }
    .width('100%')
    .height('100%')
  }

  // ===== 子组件：左侧边栏（覆盖式面板） =====
  @Builder
  Sidebar(): void {
    Column() {
      // 顶部：标题 + 关闭按钮
      Row() {
        Text('历史记录').fontSize(17).fontWeight('600').fontColor('#1F2937')
        Blank()
        Text('✕')
          .fontSize(20)
          .fontColor('#9CA3AF')
          .width(32)
          .height(32)
          .textAlign(TextAlign.Center)
          .borderRadius(16)
          .backgroundColor('#F3F4F6')
          .onClick((): void => { this.isSidebarVisible = false; })
      }
      .width('90%')
      .alignItems(VerticalAlign.Center)

      // 新建对话按钮
      Button() {
        Row({ space: 8 }) {
          Text('＋').fontSize(18).fontColor('#FFFFFF').fontWeight('700')
          Text('新建对话').fontSize(15).fontColor('#FFFFFF').fontWeight('500')
        }.alignItems(VerticalAlign.Center)
      }
      .width('90%')
      .height(44)
      .backgroundColor(PRIMARY_COLOR)
      .borderRadius(BORDER_RADIUS)
      .shadow(SHADOW_STYLE)
      .margin({ top: 16 })
      .onClick((): void => {
        this.newConversation();
        this.isSidebarVisible = false;
      })

      // 分隔线
      Divider().height(1).color('#E5E7EB').margin({ top: 16, bottom: 8 }).width('90%')

      // 历史列表
      List({ space: 4 }) {
        ForEach(this.historyList, (session: HistorySession) => {
          ListItem() {
            Row() {
              Text('💬').fontSize(16)
              Column() {
                Text(session.title)
                  .fontSize(14)
                  .fontColor(session.id === this.activeHistoryId ? PRIMARY_COLOR : '#1F2937')
                  .fontWeight(session.id === this.activeHistoryId ? 600 : 400)
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                Text(session.time).fontSize(11).fontColor('#9CA3AF')
              }
              .alignItems(HorizontalAlign.Start)
              .margin({ left: 10 })
              Blank()
              if (session.id === this.activeHistoryId) {
                Text('●').fontSize(8).fontColor(PRIMARY_COLOR)
              }
            }
            .width('100%')
            .padding(12)
            .borderRadius(10)
            .backgroundColor(session.id === this.activeHistoryId ? PRIMARY_LIGHT : 'transparent')
          }
          .onClick((): void => {
            this.restoreConversation(session.id);
            this.isSidebarVisible = false;
          })
          .gesture(
            LongPressGesture()
              .onAction((): void => {
                this.deleteHistory(session.id);
              })
          )
          .swipeAction({ end: this.DeleteButton(session.id) })
        }, (session: HistorySession): string => session.id)
      }
      .width('90%')
      .layoutWeight(1)
      .margin({ top: 8 })

      // 清空全部按钮
      if (this.historyList.length > 0) {
        Button() {
          Text('🗑 清空全部历史').fontSize(13).fontColor('#EF4444').fontWeight('500')
        }
        .width('90%')
        .height(40)
        .backgroundColor('#FEF2F2')
        .borderRadius(10)
        .margin({ top: 8, bottom: 4 })
        .onClick((): void => {
          this.clearAllHistory();
        })
      }

      // 底部：文档管理
      Divider().height(1).color('#E5E7EB').width('90%').margin({ top: 8, bottom: 8 })
      Row() {
        Text('📄 本地文档 (' + DocumentManager.getDocuments().length + ')').fontSize(13).fontColor('#6B7280').fontWeight('500')
          .onClick((): void => { this.showDocumentList(); })
        Blank()
        Text('+ 导入').fontSize(12).fontColor('#6366F1').fontWeight('600')
          .onClick((): void => { this.importDocument(); })
      }
      .width('90%')
      .padding({ top: 4, bottom: 4 })

      // 底部：LLM 模式切换 + 开发者模式
      Divider().height(1).color('#E5E7EB').width('90%').margin({ top: 8, bottom: 8 })
      Row() {
        Text(this.useLocalLlm ? '📡 纯本地模式' : '☁️ 远端模式').fontSize(13).fontColor('#6B7280').fontWeight('500')
        Blank()
        Toggle({ type: ToggleType.Switch, isOn: this.useLocalLlm })
          .width(40)
          .height(24)
          .onChange((isOn: boolean): void => {
            this.useLocalLlm = isOn;
            this.logInfo = isOn ? '已切换为纯本地模式（Qwen2.5-0.5B）' : '已切换为远端 API 模式';
          })
      }
      .width('90%')
      .padding({ top: 4, bottom: 4 })
      Row() {
        Text('🛠 开发者模式').fontSize(13).fontColor('#6B7280').fontWeight('500')
        Blank()
        Toggle({ type: ToggleType.Switch, isOn: false })
          .width(40)
          .height(24)
          .onChange((isOn: boolean): void => {
            MetricsCollector.setEnabled(isOn);
            this.showDevPanel = isOn;
            this.devPanelExpanded = false;
            this.devBallX = 16;
            this.devBallY = 200;
          })
      }
      .width('90%')
      .padding({ top: 4, bottom: 4 })
    }
    .width('80%')
    .height('100%')
    .backgroundColor(SIDEBAR_COLOR)
    .alignItems(HorizontalAlign.Center)
    .padding({ top: 20, bottom: 16 })
  }

  // 侧滑删除按钮
  @Builder
  DeleteButton(sessionId: string): void {
    Button() {
      Text('删除').fontSize(13).fontColor('#FFFFFF')
    }
    .width(60).height('100%')
    .backgroundColor('#EF4444')
    .borderRadius(8)
    .onClick((): void => {
      this.deleteHistory(sessionId);
    })
  }

  // ===== 子组件：模型选择面板 =====
  @Builder
  ModelPicker(): void {
    Column() {
      // 标题
      Row() {
        Text('选择 AI 模型').fontSize(18).fontWeight('600').fontColor('#1F2937')
        Blank()
        Text('✕')
          .fontSize(20).fontColor('#9CA3AF')
          .width(32).height(32).textAlign(TextAlign.Center).borderRadius(16)
          .backgroundColor('#F3F4F6')
          .onClick((): void => { this.isModelPickerVisible = false; })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)
      .margin({ bottom: 16 })

      // 模型列表
      List({ space: 8 }) {
        ForEach(MODEL_LIST, (model: ModelOption, idx: number) => {
          ListItem() {
            Row() {
              Column() {
                Text(model.label)
                  .fontSize(15)
                  .fontColor('#1F2937')
                  .fontWeight(idx === this.currentModelIdx ? 700 : 400)
                Text(model.modelName)
                  .fontSize(11).fontColor('#9CA3AF').margin({ top: 2 })
              }
              .alignItems(HorizontalAlign.Start)
              Blank()
              if (idx === this.currentModelIdx) {
                Text('✓').fontSize(18).fontColor(PRIMARY_COLOR).fontWeight('700')
              }
            }
            .width('100%')
            .padding(14)
            .backgroundColor(idx === this.currentModelIdx ? PRIMARY_LIGHT : '#FFFFFF')
            .borderRadius(10)
          }
          .onClick((): void => {
            this.currentModelIdx = idx;
            this.isModelPickerVisible = false;
            this.logInfo = '已切换至: ' + model.label;
          })
        }, (model: ModelOption, idx: number): string => idx.toString())
      }
      .width('100%')
      .layoutWeight(1)
    }
    .width(300)
    .height('70%')
    .backgroundColor('#FFFFFF')
    .alignItems(HorizontalAlign.Start)
    .borderRadius({ topLeft: 20, topRight: 20, bottomLeft: 0, bottomRight: 0 })
    .padding(20)
    .shadow({ radius: 16, offsetX: 0, offsetY: -4, color: 'rgba(0,0,0,0.15)' })
  }

  // ===== 子组件：顶部导航栏 =====
  @Builder
  NavBar(): void {
    Row() {
      Row({ space: 8 }) {
        Text('☰')
          .fontSize(22)
          .fontColor('#6B7280')
          .width(36)
          .height(36)
          .textAlign(TextAlign.Center)
          .borderRadius(18)
          .hoverEffect(HoverEffect.Highlight)
          .onClick((): void => { this.isSidebarVisible = !this.isSidebarVisible; })
        Text('AI')
          .fontSize(14).fontColor('#FFFFFF').fontWeight('700')
          .width(34).height(34).textAlign(TextAlign.Center)
          .background('linear-gradient(135deg, #6366F1, #8B5CF6)')
          .borderRadius(17)
        Column() {
          Text('AI 智能助手').fontSize(17).fontWeight('600').fontColor('#1F2937')
          Text('全双工语音对话').fontSize(10).fontColor('#9CA3AF').margin({ top: 1 })
        }.alignItems(HorizontalAlign.Start).margin({ left: 2 })
      }
      .alignItems(VerticalAlign.Center)

      Blank()

      // 右侧：用户区
      Row({ space: 6 }) {
        // 当前对话删除按钮（历史记录模式时显示）
        if (this.activeHistoryId) {
          Text('🗑')
            .fontSize(15)
            .width(32).height(32)
            .textAlign(TextAlign.Center)
            .backgroundColor('#FEF2F2')
            .borderRadius(16)
            .onClick((): void => {
              this.deleteHistory(this.activeHistoryId);
              this.logInfo = '已删除当前对话';
            })
        }
        // 模型选择
        Row({ space: 4 }) {
          Text('⚡').fontSize(11)
          Text(MODEL_LIST[this.currentModelIdx].label).fontSize(11).fontColor(PRIMARY_COLOR)
        }
        .backgroundColor(PRIMARY_LIGHT)
        .borderRadius(10)
        .padding({ left: 8, right: 8, top: 4, bottom: 4 })
        .onClick((): void => { this.isModelPickerVisible = true; })
        // 录音状态指示
        if (this.isRecording) {
          Circle()
            .width(10).height(10)
            .fill('#EF4444')
            .shadow({ radius: 4, offsetX: 0, offsetY: 0, color: 'rgba(239,68,68,0.5)' })
        }
      }
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .height(NAV_HEIGHT)
    .padding({ left: 16, right: 16 })
    .backgroundColor('#FFFFFF')
    .shadow({ radius: 4, offsetX: 0, offsetY: 1, color: 'rgba(0,0,0,0.05)' })
  }

  // ===== 子组件：欢迎区域 =====
  @Builder
  WelcomeSection(): void {
    Column() {
      Blank().height(40)
      // 渐变欢迎卡片
      Column() {
        Blank().height(30)
        Text('🤖').fontSize(48)
        Text('您好！我是 AI 智能助手')
          .fontSize(22).fontWeight('600').fontColor('#FFFFFF').margin({ top: 12 })
        Text('支持语音输入 · 文字对话 · 语音合成')
          .fontSize(13).fontColor('rgba(255,255,255,0.75)').margin({ top: 6 })
        Text('点击 🎤 开始语音输入，或直接输入文字')
          .fontSize(12).fontColor('rgba(255,255,255,0.55)').margin({ top: 4 })
        Blank().height(30)
      }
      .width('92%')
      .background('linear-gradient(135deg, #6366F1, #8B5CF6, #A855F7)')
      .borderRadius(20)
      .shadow({ radius: 16, offsetX: 0, offsetY: 6, color: 'rgba(99,102,241,0.3)' })
      .alignItems(HorizontalAlign.Center)
      // 快捷提问卡片
      Row({ space: 12 }) {
        this.SuggestionCard('💡', '写一份周报')
        this.SuggestionCard('📝', '代码优化建议')
        this.SuggestionCard('🎯', '学习路线规划')
      }
      .margin({ top: 28 })
    }
    .width('100%')
    .alignItems(HorizontalAlign.Center)
  }

  @Builder
  SuggestionCard(icon: string, label: string): void {
    Column() {
      Text(icon).fontSize(28)
      Text(label).fontSize(12).fontColor('#4B5563').margin({ top: 6 }).fontWeight('500')
    }
    .width(95)
    .height(85)
    .backgroundColor('#FFFFFF')
    .borderRadius(14)
    .shadow({ radius: 6, offsetX: 0, offsetY: 2, color: 'rgba(0,0,0,0.06)' })
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
    .onClick((): void => {
      this.sendQuickMsg(label);
    })
  }

  private sendQuickMsg(label: string): void {
    const userMsg: ChatItem = { msg: `帮我${label.toLowerCase()}`, from: 'user' };
    this.chatList.push(userMsg);
    this.scrollToBottom();
    setTimeout((): void => {
      const aiMsg: ChatItem = { msg: `好的，我帮您${label}，请稍等…`, from: 'assistant' };
      this.chatList.push(aiMsg);
      this.scrollToBottom();
    }, 600);
  }

  // ===== 子组件：底部输入区 =====
  @Builder
  BottomInput(): void {
    Column() {
      // 录音状态提示条
      if (this.isRecording || this.fullDuplexActive) {
        Row() {
          Circle().width(6).height(6).fill('#EF4444')
          Text(this.isRecording ? '🎤 录音中…' : '🔴 全双工模式').fontSize(12).fontColor('#6B7280').margin({ left: 6 })
          Blank()
          if (!this.fullDuplexActive) {
            Text('点击 ⏹ 停止').fontSize(11).fontColor('#9CA3AF')
          }
        }
        .width('92%')
        .padding({
          left: 12,
          right: 12,
          top: 6,
          bottom: 6
        })
        .backgroundColor('#FEF2F2')
        .borderRadius(8)
        .margin({ bottom: 4 })
      }
      Row() {
        Button() {
          Circle().width(36).height(36)
            .fill(this.isRecording ? '#EF4444' : PRIMARY_COLOR)
            .overlay(this.isRecording ? '⏹' : '🎤', { align: Alignment.Center })
        }
        .width(44)
        .height(44)
        .backgroundColor('#FFFFFF')
        .borderRadius(22)
        .onClick((): void => this.toggleRecord())

        TextArea({
          placeholder: '输入消息…',
          text: this.inputText
        })
          .height(44)
          .layoutWeight(1)
          .margin({ left: 8, right: 8 })
          .backgroundColor('#F3F4F6')
          .borderRadius(22)
          .padding({ left: 16, right: 16 })
          .fontSize(15)
          .onChange((val: string): void => {
            this.inputText = val;
          })

        Button() {
          Circle().width(40).height(40)
            .fill(this.inputText.trim() ? '#6366F1' : '#D1D5DB')
            .overlay('↑', { align: Alignment.Center })
        }
        .width(44)
        .height(44)
        .backgroundColor('#FFFFFF')
        .borderRadius(22)
        .enabled(this.inputText.trim().length > 0)
        .onClick((): void => this.sendTextMsg())

        Button() {
          Text(this.fullDuplexActive ? '🔴' : '🔇').fontSize(16)
        }
        .width(36)
        .height(36)
        .backgroundColor(this.fullDuplexActive ? '#FEE2E2' : '#F3F4F6')
        .borderRadius(18)
        .margin({ left: 4 })
        .onClick((): void => this.toggleFullDuplex())
      } // Row end
      if (this.fullDuplexActive) {
        Text('状态: ' + this.fullDuplexState).fontSize(10).fontColor('#EF4444').margin({ left: 16 })
      }
      if (this.logInfo) {
        Text(this.logInfo).fontSize(12).fontColor('#9CA3AF').margin({ left: 16, top: 4 })
      }
    }
    .width('100%')
    .padding({
      left: 12,
      right: 12,
      top: 8,
      bottom: 8
    })
    .backgroundColor('#FFFFFF')
  }
  }