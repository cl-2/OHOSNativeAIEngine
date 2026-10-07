/*
 * Index.ets – 本地语音助手
 * 自然呼吸主题：对话卡片 + 自适应语音交互坞
 */
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { connection } from '@kit.NetworkKit';
import nativeLib from 'libnative_lib.so';
import { getAbilityContext as getAbCtx } from '../utils/ContextHolder';
import MessageBubble from './MessageBubble.ets';
import { MetricsCollector } from '../utils/MetricsCollector';
import { MetricTypeId } from '../utils/MetricsCollector';
import { MemoryManager, MemoryPreferenceItem } from '../utils/MemoryManager';
import { DocumentManager } from '../utils/DocumentManager';
import { BenchmarkRunner } from '../utils/BenchmarkRunner';
import { CryptoManager } from '../utils/CryptoManager';
import { sanitizeLog } from '../utils/LogSanitizer';
import { DevMetricsPanel } from './DevMetricsPanel.ets';
import {
  ROUTING_LOCAL,
  ROUTING_REMOTE,
  ROUTING_SMART
} from '../utils/LlmRoutePolicy';
import { LocalReminderRecord, ReminderResult, ReminderService } from '../utils/ReminderService';
import { MemoRecord, MemoResult, MemoService } from '../utils/MemoService';
import { DeterministicSkillRoute, routeDeterministicSkill } from '../utils/DeterministicSkillRouter';
import { RuntimeMemoryManager } from '../utils/RuntimeMemoryManager';
import { PrivacyAuditItem, PrivacyAuditReport } from '../utils/PrivacyAuditRunner';
import { LlmConversationContext, LlmContextMessage } from '../controllers/LlmConversationContext';
import {
  isRemoteConfigComplete,
  LlmSettingsStore,
  normalizeRemoteUrl,
  StoredRemoteLlmConfig,
  StoredRoutingMode
} from '../controllers/LlmSettingsStore';
import {
  PersonalDataController,
  PersonalDataOverview,
  PersonalMemoryKind
} from '../controllers/PersonalDataController';
import {
  LocalControlResult,
  LocalOperationResult,
  LocalSkillController
} from '../controllers/LocalSkillController';
import {
  LlmRequestOrchestrator,
  RemoteStateUpdate
} from '../controllers/LlmRequestOrchestrator';
import {
  LocalLlmRuntimeController,
  LocalLlmRuntimeCallbacks,
  LocalLlmModelInfo
} from '../controllers/LocalLlmRuntimeController';
import {
  ConversationChatItem,
  ConversationRestoreResult,
  ConversationSessionController,
  ConversationSessionItem
} from '../controllers/ConversationSessionController';
import { SpeechModelInstaller } from '../services/SpeechModelInstaller';
import {
  AsrPrewarmEvent,
  AsrWorkerEvent,
  SpeechWorkerCoordinator,
  TtsWorkerEvent
} from '../services/SpeechWorkerCoordinator';

// ===== 数据类型 =====
interface ChatItem {
  msg: string;
  from: 'user' | 'assistant';
  source?: string;
}

interface HistorySession {
  id: string;
  title: string;
  time: string;
}

// ===== 阴影样式接口（匹配 ShadowOptions） =====
interface ShadowConfig {
  radius: number;
  offsetX: number;
  offsetY: number;
  color: string;
}

// ===== 主题常量 =====
const PRIMARY_COLOR: string = '#45AFA8';
const PRIMARY_DARK: string = '#278C87';
const PRIMARY_LIGHT: string = '#EAF7F4';
const BG_COLOR: string = '#F7FAF8';
const SIDEBAR_COLOR: string = '#FFFFFF';
const NAV_HEIGHT: number = 68;
const BORDER_RADIUS: number = 16;
const SHADOW_STYLE: ShadowConfig = { radius: 14, offsetX: 0, offsetY: 4, color: 'rgba(32,91,82,0.09)' };
// TTS 重新加载比本地 LLM 轻得多；30 秒无人播放后释放模型，优先把约 350MB
// 的可回收内存还给前台应用，同时保留短暂停顿内的连续对话体验。
const TTS_IDLE_RELEASE_MS: number = 30000;

// ===== LLM 相关类型和配置 =====
type LlmMessage = LlmContextMessage;

interface ModelOption {
  label: string;
  modelName: string;
  apiUrl: string;
  kind: 'ollama' | 'cloud';
}

const MODEL_LIST: ModelOption[] = [
  { label: '自建 Ollama', modelName: 'qwen2.5:7b', apiUrl: '', kind: 'ollama' },
  { label: 'DeepSeek', modelName: 'deepseek-chat', apiUrl: 'https://api.deepseek.com/v1/chat/completions', kind: 'cloud' },
  { label: 'OpenAI 兼容 API', modelName: 'gpt-4o-mini', apiUrl: 'https://api.openai.com/v1/chat/completions', kind: 'cloud' },
  { label: '通义千问', modelName: 'qwen-plus', apiUrl: 'https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions', kind: 'cloud' },
  { label: '智谱 GLM-4', modelName: 'glm-4', apiUrl: 'https://open.bigmodel.cn/api/paas/v4/chat/completions', kind: 'cloud' },
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
  @State isReminderPanelVisible: boolean = false;
  @State reminderItems: LocalReminderRecord[] = [];
  @State isMemoPanelVisible: boolean = false;
  @State memoItems: MemoRecord[] = [];
  @State isPrivacyPanelVisible: boolean = false;
  @State privacyFacts: string[] = [];
  @State privacyPreferences: MemoryPreferenceItem[] = [];
  @State privacyTopics: string[] = [];
  @State privacyConversationCount: number = 0;
  @State privacyReminderCount: number = 0;
  @State privacyStatus: string = '';
  @State privacyBusy: boolean = false;
  @State privacyAuditItems: PrivacyAuditItem[] = [];
  @State privacyAuditSummary: string = '';
  @State privacyAuditRunning: boolean = false;
  @State activeHistoryId: string = '';
  @State modelStatus: string = '';
  @State modelsReady: boolean = false;

  // 全双工模式状态
  @State fullDuplexActive: boolean = false;
  @State fullDuplexState: string = 'IDLE';
  @State uiRefreshKey: number = 0; // 强制刷新 UI 用
  private _appStartTime: number = Date.now();  // App 启动时间戳，用于计时日志
  private duplexTimer: number = -1;
  private ttsPendingCount: number = 0;       // 流式 TTS 待播句数
  private speechWorkers: SpeechWorkerCoordinator = new SpeechWorkerCoordinator();


  @State useLocalLlm: boolean = true;         // true=本地模型, false=用户配置的远端 API
  @State llmRoutingMode: number = ROUTING_LOCAL;
  // 本地 LLM 状态
  private localLlmController: LocalLlmRuntimeController = new LocalLlmRuntimeController();
  private llmContext: LlmConversationContext = new LlmConversationContext();
  private localSkillController: LocalSkillController = new LocalSkillController();
  private llmSettingsStore?: LlmSettingsStore;
  private personalDataController?: PersonalDataController;
  private llmRequestOrchestrator: LlmRequestOrchestrator = new LlmRequestOrchestrator();
  private conversationController: ConversationSessionController = new ConversationSessionController();
  @State localLlmStatus: string = '检查中…';
  @State localLlmState: string = 'UNAVAILABLE';
  @State dlProgress: number = 0;               // 下载进度 0~100
  @State dlSpeed: number = 0;                  // 下载速度 KB/s
  @State isDownloading: boolean = false;
  @State isModelVerifying: boolean = false;
  @State localModelFilePresent: boolean = false;
  @State localModelDetails: string = '尚未安装';
  private downloadCancelledByUser: boolean = false;
  private isOnline: boolean = true; // 当前网络状态
  private manualLocalLlm: boolean = true;
  private netConnection?: connection.NetConnection;
  private ttsIdleReleaseTimer: number = -1;
  private scrollTimer: number = -1;
  private llmInterruptedContext: string = ''; // 打断时 assistant 的部分回复（增量上下文）
  @State asrInterimText: string = '';         // 实时语音识别文字
  @State keyboardInputVisible: boolean = false; // 语音坞中的文字输入切换（仅 UI）
  @State breathExpanded: boolean = false;       // 呼吸球动画状态（仅 UI）
  @State remoteApiUrl: string = '';
  @State remoteApiKey: string = '';
  @State remoteModelName: string = 'qwen2.5:7b';
  @State remoteConfigStatus: string = '未配置';
  @State remoteConfigVerified: boolean = false;

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
  private breathTimer: number = -1;

  // 全双工模式切换（按钮触发，不依赖唤醒词）
  private toggleFullDuplex(): void {
    if (this.fullDuplexActive) {
      console.info('OHOS_FLOW: toggleFullDuplex OFF');
      nativeLib.stopFullDuplex();
      this.fullDuplexActive = false;
      this.fullDuplexState = 'IDLE';
      this.logInfo = '全双工已关闭';
      clearInterval(this.duplexTimer);
      // 停止持续录音
      if (this.isRecording || this.speechWorkers.hasAsrWorker) {
        this.stopAsrInternal();
        this.isRecording = false;
      }
      // 重新启用后台唤醒词监听

    } else {
      console.info('OHOS_FLOW: toggleFullDuplex ON');
      this.lastFinalText = '';
      try {
        // 按钮模式 → 不依赖唤醒词，直接进入对话循环
        nativeLib.startFullDuplex();
        this.fullDuplexActive = true;
        this.logInfo = '🎤 全双工模式';
        this.duplexTimer = setInterval((): void => {
          this.fullDuplexState = nativeLib.getFullDuplexState() as string;

        }, 500);
        setTimeout((): void => this.toggleRecord(), 200);
      } catch (e) {
        this.logInfo = '全双工启动失败: ' + String(e);
      }
    }
  }

  // 自动启动后台唤醒词监听（App 启动时自动调用，无需用户点击）
  // 从 EntryAbility 保存的 AbilityContext（用于 C++ rawfile API）
  private getAbilityContext(): Object | undefined {
    return getAbCtx() as Object | undefined;
  }

  private handleNetworkState(online: boolean): void {
    const changed: boolean = this.isOnline !== online;
    this.isOnline = online;
    if (online) {
      if (changed) {
        this.logInfo = this.llmRoutingMode === ROUTING_LOCAL ? '网络已恢复，继续使用本地模型' :
          (this.llmRoutingMode === ROUTING_SMART ? '网络已恢复，智能路由将在下一轮重新判定' :
            '网络已恢复，远端配置将在下一轮重试');
      }
      console.info('OHOS_NET: available manualLocal=' + this.manualLocalLlm);
      return;
    }

    this.cancelLocalLlmRelease();
    if (this.llmRoutingMode === ROUTING_SMART && this.localLlmController.isInstalled) {
      this.logInfo = '网络已断开：普通任务仍可本地处理，事实问题将拒绝臆测';
      console.info('OHOS_NET: unavailable, smart routing keeps private/general tasks local');
    } else if (!this.useLocalLlm && this.localLlmController.isInstalled) {
      this.logInfo = '网络已断开，本轮将自动回退本地模型';
      console.info('OHOS_NET: unavailable, local model selected for next request');
    } else if (!this.localLlmController.isInstalled) {
      this.logInfo = '网络已断开，本地模型尚未下载';
      console.info('OHOS_NET: unavailable, no local model installed');
    }
  }

  private startNetworkMonitor(): void {
    if (this.netConnection) return;
    try {
      const monitor: connection.NetConnection = connection.createNetConnection();
      monitor.on('netAvailable', (_handle: connection.NetHandle): void => {
        this.handleNetworkState(true);
      });
      monitor.on('netLost', (_handle: connection.NetHandle): void => {
        this.handleNetworkState(false);
      });
      monitor.on('netUnavailable', (): void => {
        this.handleNetworkState(false);
      });
      monitor.register((err: object): void => {
        if (err) {
          console.warn('OHOS_NET: monitor register failed=' + JSON.stringify(err));
        }
      });
      this.netConnection = monitor;
      connection.getDefaultNet((err: object, netHandle: connection.NetHandle): void => {
        this.handleNetworkState(!err && !!netHandle);
      });
    } catch (e) {
      console.warn('OHOS_NET: monitor start failed=' + String(e));
    }
  }

  private stopNetworkMonitor(): void {
    const monitor: connection.NetConnection | undefined = this.netConnection;
    this.netConnection = undefined;
    if (!monitor) return;
    try {
      monitor.unregister((err: object): void => {
        if (err) console.warn('OHOS_NET: monitor unregister failed=' + JSON.stringify(err));
      });
    } catch (e) {
      console.warn('OHOS_NET: monitor stop failed=' + String(e));
    }
  }

  private settingsStore(): LlmSettingsStore {
    if (!this.llmSettingsStore) this.llmSettingsStore = new LlmSettingsStore(getContext().filesDir);
    return this.llmSettingsStore;
  }

  private personalData(): PersonalDataController {
    if (!this.personalDataController) {
      this.personalDataController = new PersonalDataController(getContext().filesDir);
    }
    return this.personalDataController;
  }

  private async loadRoutingMode(): Promise<void> {
    const saved: StoredRoutingMode | null = await this.settingsStore().loadRoutingMode();
    if (!saved || saved.mode < ROUTING_LOCAL || saved.mode > ROUTING_REMOTE) return;
    this.llmRoutingMode = saved.mode;
    this.useLocalLlm = saved.mode === ROUTING_LOCAL;
    this.manualLocalLlm = saved.mode === ROUTING_LOCAL;
  }

  private persistRoutingMode(): void {
    this.settingsStore().saveRoutingMode(this.llmRoutingMode).catch((e: Error): void => {
      console.warn('OHOS_ROUTE: persist mode failed=' + String(e));
    });
  }

  private routingModeLabel(): string {
    if (this.llmRoutingMode === ROUTING_SMART) return '🧭 智能路由';
    if (this.llmRoutingMode === ROUTING_REMOTE) return '☁️ 指定远端';
    return '📡 纯本地';
  }

  private setRoutingMode(mode: number): void {
    if (mode < ROUTING_LOCAL || mode > ROUTING_REMOTE) return;
    if (mode === ROUTING_REMOTE && !this.isRemoteConfigured()) {
      this.logInfo = '请先配置远端 API 或自建 Ollama';
      this.isModelPickerVisible = true;
      return;
    }
    this.llmRoutingMode = mode;
    this.useLocalLlm = mode === ROUTING_LOCAL;
    this.manualLocalLlm = mode === ROUTING_LOCAL;
    this.persistRoutingMode();

    if (mode === ROUTING_LOCAL) {
      this.cancelLocalLlmRelease();
      if (!this.localLlmController.isInstalled) {
        this.logInfo = '纯本地模式需要先下载本机 GGUF 模型';
      } else if (!this.localLlmController.isReady) {
        this.logInfo = '正在加载本地模型…';
        this.prepareLocalLlm();
      } else {
        this.logInfo = '📡 已切换纯本地模式';
      }
      return;
    }

    if (mode === ROUTING_SMART) {
      this.logInfo = this.isRemoteConfigured() ?
        '🧭 智能路由：隐私/普通任务本地，事实问题远端' :
        '🧭 智能路由：未配置远端，事实问题将拒绝臆测';
      return;
    }

    this.logInfo = '☁️ 已切换指定远端模式';
    this.scheduleLocalLlmRelease();
  }

  private async loadRemoteLlmConfig(): Promise<void> {
    const saved: StoredRemoteLlmConfig | null = await this.settingsStore().loadRemoteConfig();
    if (!saved) return;
    const index: number = saved.providerIndex;
    if (index < 0 || index >= MODEL_LIST.length) return;
    this.currentModelIdx = index;
    this.remoteApiUrl = saved.apiUrl || '';
    this.remoteModelName = saved.modelName || MODEL_LIST[index].modelName;
    this.remoteApiKey = saved.apiKey || '';
    this.remoteConfigVerified = false;
    this.remoteConfigStatus = this.isRemoteConfigured() ? '已配置，待连接验证' : '配置不完整';
  }

  private saveRemoteLlmConfig(): void {
    if (!this.isRemoteConfigured()) {
      this.remoteConfigStatus = this.isOllamaSelected() ? '请填写 Ollama 服务地址' : '请填写服务地址、模型名和 API Key';
      this.logInfo = this.remoteConfigStatus;
      return;
    }
    const normalizedUrl: string = this.normalizedRemoteUrl();
    this.remoteApiUrl = normalizedUrl;
    const config: StoredRemoteLlmConfig = {
      providerIndex: this.currentModelIdx,
      apiUrl: normalizedUrl,
      modelName: this.remoteModelName.trim(),
      apiKey: this.remoteApiKey.trim(),
    };
    this.settingsStore().saveRemoteConfig(config).then((): void => {
      this.remoteConfigVerified = false;
      this.remoteConfigStatus = '已保存，首次请求将自动验证';
      this.logInfo = '远端配置已保存，可切换远端模式';
    });
  }

  private isOllamaSelected(): boolean {
    return MODEL_LIST[this.currentModelIdx].kind === 'ollama';
  }

  private isRemoteConfigured(): boolean {
    const config: StoredRemoteLlmConfig = {
      providerIndex: this.currentModelIdx,
      apiUrl: this.remoteApiUrl,
      modelName: this.remoteModelName,
      apiKey: this.remoteApiKey
    };
    return isRemoteConfigComplete(config, this.isOllamaSelected());
  }

  // Ollama 的 OpenAI 兼容接口固定为 /v1/chat/completions。用户只填写 IP
  // 或 IP:端口时，在请求前补全协议、默认 11434 端口和接口路径；完整 URL 保持不变。
  private normalizedRemoteUrl(): string {
    return normalizeRemoteUrl(this.remoteApiUrl, this.isOllamaSelected());
  }

  private selectRemoteProvider(index: number): void {
    this.currentModelIdx = index;
    const model: ModelOption = MODEL_LIST[index];
    this.remoteModelName = model.modelName;
    this.remoteApiUrl = model.apiUrl;
    this.remoteApiKey = '';
    this.remoteConfigVerified = false;
    this.remoteConfigStatus = model.kind === 'ollama' ? '请填写局域网或自建服务地址' : '请填写您自己的 API Key';
  }

  private remoteDisplayLabel(): string {
    if (this.llmRoutingMode === ROUTING_SMART) return '智能路由';
    if (this.llmRoutingMode === ROUTING_LOCAL) return '本机 GGUF';
    const model: ModelOption = MODEL_LIST[this.currentModelIdx];
    return this.isRemoteConfigured() ? model.label : '配置远端模型';
  }

  // 模拟历史对话数据（启动时从本地加载）
  @State historyList: HistorySession[] = [];

  // ---------- 内部引用 ----------
  private scrollRef: Scroller = new Scroller();

  // ---------- 生命周期 ----------
  aboutToAppear(): void {
    // 初始化 TTS worker
    this.speechWorkers.initializeTts((event: TtsWorkerEvent): void => {
      const type: string = event.type;
      if (type === 'stopped') {
        const reason: string = event.reason;
        this.ttsPendingCount = 0;
        this.logInfo = 'TTS stopped';
        RuntimeMemoryManager.capture(reason === 'interrupted' ? 'TTS打断后' : 'TTS播放完成');
        this.scheduleTtsIdleRelease();
        if (this.fullDuplexActive) {
          nativeLib.notifyDuplexEvent(reason === 'interrupted' ? 'tts_stopped' : 'tts_complete', '');
        }
      }
      if (type === 'rendererReleased') {
        RuntimeMemoryManager.capture('TTS引擎与渲染器释放后');
      }
    });

    // rawfile 在 HAP 内部，C++ fopen 无法直接读取，需要复制到沙箱
    this.modelStatus = '正在加载模型…(首次约10秒)';
    // 初始化性能监控
    MetricsCollector.init(getContext().filesDir);
    RuntimeMemoryManager.init(getContext().filesDir);
    RuntimeMemoryManager.capture('应用启动');
    // 初始化 LLM 管理器（传入驻正确的沙箱路径）
    this.localLlmController.init(getContext().filesDir + '/models/llm');
    this.refreshLocalModelDetails();
    MemoryManager.init(getContext().filesDir).catch((e: Error): void => {
      console.error('OHOS_Memory: initialization failed=' + String(e));
    });
    DocumentManager.init(getContext().filesDir);
    BenchmarkRunner.init(getContext().filesDir);
    ReminderService.init(getContext().filesDir, getContext() as common.UIAbilityContext,
      (content: string): void => this.onReminderTriggered(content)).then((): void => {
      this.refreshReminderItems();
    }).catch((e: Error): void => {
      console.error('OHOS_REMINDER: init failed=' + String(e));
    });
    MemoService.init(getContext().filesDir).then((): void => {
      this.refreshMemoItems();
    }).catch((e: Error): void => {
      console.error('OHOS_MEMO: init failed=' + String(e));
    });
    this.conversationController.init(getContext().filesDir);
    CryptoManager.init().then((): Promise<boolean> => CryptoManager.selfTest()).then((passed: boolean): void => {
      console.info('OHOS_CRYPTO: AES-256-GCM roundtrip/tamper self-test=' + passed);
    }).catch((e: Error): void => {
      console.error('OHOS_CRYPTO: initialization/self-test failed=' + String(e));
    });
    this.loadRemoteLlmConfig();
    this.loadRoutingMode();
    MetricsCollector.recordNewConversation();
    this.devPollTimer = setInterval((): void => {
      if (RuntimeMemoryManager.consumeAppBackground()) this.handleAppBackground();
      if (RuntimeMemoryManager.consumeAppForeground()) RuntimeMemoryManager.capture('回到前台');
      const memoryLevel: number = RuntimeMemoryManager.consumeMemoryPressure();
      if (memoryLevel >= 0) this.handleMemoryPressure(memoryLevel);
      if (this.showDevPanel && !this.devPanelExpanded) {
        try {
          const rtf: number = MetricsCollector.getLatest(MetricTypeId.AsrRtf);
          this.devLatestRtf = rtf;
          this.devRtfColor = rtf < 0.2 ? '#10B981' : rtf < 0.5 ? '#F59E0B' : '#EF4444';
        } catch (_) {}
      }
    }, 800);
    this.loadHistoryList();
    // 后台预热 TTS 引擎（不依赖模型复制，可直接启动）
    try {
      nativeLib.startStreamingTtsWithSpeed('', this.prepareModelDir(), 1.0, () => {});
    } catch (_) {}
    // 异步复制模型 → 完成后预热 ASR 引擎
    this.ensureAsrModels().then((): void => {
      // 模型就绪后后台预热 ASR 引擎
      try {
        this.speechWorkers.prewarmAsr(this.prepareAsrModelDir(), this.prepareVadModelPath(),
          (event: AsrPrewarmEvent): void => {
          if (event.success) {
            RuntimeMemoryManager.capture('ASR预热完成(' + event.durationMs + 'ms)', true);
          } else {
            console.warn('OHOS_load: ASR prewarm failed: ' + event.error);
            RuntimeMemoryManager.capture('ASR预热失败', true);
          }
        });
      } catch (_e) {
        console.warn('OHOS_Native_ai_engine: ASR prewarm failed: ' + String(_e));
      }
    });
    // 注册全双工 Action 回调（状态机 → ArkTS）
    try {
      nativeLib.setDuplexCallbacks((action: string, payload: string): void => {
        console.info('OHOS_FLOW: duplex action=' + action + ' payload="' + payload.substring(0, 50) + '"');
        if (action === 'llm_request') {
          // 状态机要求调用 LLM → 清空 ASR 中间结果
          this.asrInterimText = '';
          this.onLlmRequested(payload);
        } else if (action === 'asr_interim') {
          this.asrInterimText = payload;
          this.logInfo = '🎤 识别中: ' + payload;
        } else if (action === 'stop_tts') {
          this.speechWorkers.sendTtsCommand('stopTts');
        } else if (action === 'duck_tts') {
          this.speechWorkers.sendTtsCommand('duckTts');
        } else if (action === 'resume_tts') {
          this.speechWorkers.sendTtsCommand('resumeTts');
        } else if (action === 'overlap_start') {
          // 全双工：用户说话时 AI 继续播放，同时捕获用户语音
          console.info('OHOS_FD: overlap START - user speaking during AI response');
          // 当前架构在整段 LLM 回复完成后才启动 TTS，因此被打断的回复已经存在
          // 于 LLM 上下文控制器。不要再把同一回复拼进下一条 user prompt，否则小模型会
          // 被重复答案强烈锚定并在后续轮次持续复读。
          this.llmInterruptedContext = '';
          this.logInfo = '🔊 已听到您的输入，听完后继续…';
        } else if (action === 'overlap_end') {
          console.info('OHOS_FD: overlap END - user finished');
          this.logInfo = '📝 处理您的输入…';
        } else if (action === 'interrupted') {
          this.lastFinalText = '';
          this.asrInterimText = '';
          this.logInfo = '🔊 检测到打断';
          // 回复已在常规历史中，只清理旧的临时上下文，避免重复注入。
          this.llmInterruptedContext = '';
        }
      });
      console.info('OHOS_FLOW: duplex callbacks registered');
    } catch (_e) {
      console.warn('OHOS_FLOW: setDuplexCallbacks failed: ' + String(_e));
    }
    // 网络检测：只反映网络可用性；远端服务是否可达由实际请求验证。
    this.startNetworkMonitor();

    // 纯视觉呼吸动画，不参与录音、状态机或音频时序。
    this.breathTimer = setInterval((): void => {
      this.breathExpanded = !this.breathExpanded;
    }, 1500);

  }

  aboutToDisappear(): void {
    this.saveCurrentConversation();
    this.stopNetworkMonitor();
    this.localLlmController.cancelIdleRelease();
    if (this.ttsIdleReleaseTimer >= 0) {
      clearTimeout(this.ttsIdleReleaseTimer);
      this.ttsIdleReleaseTimer = -1;
    }
    if (this.scrollTimer >= 0) {
      clearTimeout(this.scrollTimer);
      this.scrollTimer = -1;
    }
    this.localLlmController.releaseRuntime();
    if (this.devPollTimer >= 0) {
      clearInterval(this.devPollTimer);
      this.devPollTimer = -1;
    }
    if (this.breathTimer >= 0) {
      clearInterval(this.breathTimer);
      this.breathTimer = -1;
    }

  }

  // ---------- 模型资源初始化 ----------
  private async ensureAsrModels(): Promise<void> {
    try {
      const context: common.UIAbilityContext = getContext() as common.UIAbilityContext;
      const installer: SpeechModelInstaller = new SpeechModelInstaller(context, this.getAbilityContext());
      const sandboxDir: string = await installer.ensureInstalled();

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
      // 后台检测本地 LLM 模型并预热
      this.localLlmController.startupCheck((msg: string): void => {
        this.localLlmStatus = msg;
        this.localLlmState = this.localLlmController.state;
        this.refreshLocalModelDetails();
      });
    } catch (e) {
      console.error('OHOS_Native_ai_engine: Model error: ' + String(e));
      this.modelStatus = '模型加载失败: ' + String(e);
    }
  }

  // ---------- ASR ----------
  private async startAsr(): Promise<void> {
    console.info('OHOS_FLOW: startAsr called, modelsReady=' + this.modelsReady +
      ' workerExists=' + this.speechWorkers.hasAsrWorker +
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
    this.speechWorkers.startAsr(this.prepareAsrModelDir(), this.prepareVadModelPath(),
      (event: AsrWorkerEvent): void => {
        console.info('OHOS_Native_ai_engine: Index received worker msg: ' + event.type);
        if (event.type === 'asr-result') {
          this.handleAsrResult(event.text, event.isFinal);
        } else if (event.type === 'asr-stopped') {
          this.isRecording = false;
          this.logInfo = event.error ? '❌ ' + event.error : '录音已停止';
        }
      });
    this.isRecording = true;
    this.logInfo = '正在录音…';
    setTimeout((): void => {
      RuntimeMemoryManager.capture('ASR运行中');
    }, 500);
  }

  private lastFinalText: string = '';

  private buildLlmPrelude(userText: string, remoteMode: boolean = false): LlmMessage[] {
    const memoryPrompt: string = MemoryManager.getMemoryPrompt();
    const ragContext: string = DocumentManager.buildContext(userText);
    return this.llmContext.buildPrelude(userText, memoryPrompt, ragContext, remoteMode);
  }

  private rebuildLlmHistoryFromChat(): void {
    this.llmContext.rebuild(this.chatList);
  }

  @State currentModelIdx: number = 0;
  @State isModelPickerVisible: boolean = false;

  private cancelLocalLlmRelease(): void {
    this.localLlmController.cancelIdleRelease();
  }

  private scheduleLocalLlmRelease(): void {
    this.localLlmController.scheduleIdleRelease(
      (): boolean => !this.useLocalLlm && this.isOnline,
      (state: string): void => {
      this.localLlmState = state;
      this.localLlmStatus = '本地模型已安装（未加载）';
      }
    );
  }

  private cancelTtsIdleRelease(): void {
    if (this.ttsIdleReleaseTimer >= 0) {
      clearTimeout(this.ttsIdleReleaseTimer);
      this.ttsIdleReleaseTimer = -1;
    }
  }

  private scheduleTtsIdleRelease(): void {
    this.cancelTtsIdleRelease();
    this.ttsIdleReleaseTimer = setTimeout((): void => {
      this.ttsIdleReleaseTimer = -1;
      if (this.ttsPendingCount > 0 || this.fullDuplexState === 'TTS_PLAYING') return;
      try {
        const released: boolean = nativeLib.releaseTtsEngine();
        if (released) {
          // The worker's AudioRenderer has independent native buffers. It
          // must be released alongside the TTS engine to reclaim idle memory.
          this.speechWorkers.releaseTtsRenderer();
        }
      } catch (e) {
        console.warn('OHOS_MEMORY: release idle TTS failed=' + String(e));
      }
    }, TTS_IDLE_RELEASE_MS);
  }

  private handleMemoryPressure(level: number): void {
    console.warn('OHOS_MEMORY: handling pressure level=' + level +
      ' localRequests=' + this.localLlmController.activeRequestCount + ' ttsPending=' + this.ttsPendingCount);
    // Moderate pressure only evicts an idle fallback model in remote mode.
    // Low/critical pressure may evict it in local mode; the next request lazily reloads it.
    if ((level >= 1 || !this.useLocalLlm) && this.localLlmController.releaseIfIdle()) {
      this.localLlmState = this.localLlmController.state;
    }
    if (level >= 1 && this.ttsPendingCount === 0 && this.fullDuplexState !== 'TTS_PLAYING') {
      this.cancelTtsIdleRelease();
      try {
        if (nativeLib.releaseTtsEngine()) {
          this.speechWorkers.releaseTtsRenderer();
        }
      } catch (_) {}
    }
    RuntimeMemoryManager.capture(level >= 2 ? '严重内存压力回收后' :
      (level === 1 ? '低内存回收后' : '中等内存压力回收后'));
    this.logInfo = level >= 2 ? '系统内存紧张，已释放闲置模型' : this.logInfo;
  }

  private handleAppBackground(): void {
    if (this.localLlmController.releaseIfIdle()) {
      this.localLlmState = this.localLlmController.state;
    }
    if (this.ttsPendingCount === 0 && this.fullDuplexState !== 'TTS_PLAYING') {
      this.cancelTtsIdleRelease();
      try {
        if (nativeLib.releaseTtsEngine()) {
          this.speechWorkers.releaseTtsRenderer();
        }
      } catch (_) {}
    }
    // ASR/VAD deliberately stay resident: background voice mode and barge-in
    // must not lose their low-latency path.
    RuntimeMemoryManager.capture('进入后台回收后');
  }

  private prepareLocalLlm(): void {
    this.cancelLocalLlmRelease();
    this.localLlmController.ensureReady((msg: string): void => {
      this.localLlmStatus = msg;
      this.localLlmState = this.localLlmController.state;
    }).then((ok: boolean): void => {
      this.localLlmState = this.localLlmController.state;
      this.logInfo = ok ? '本地模型已就绪' : '本地模型加载失败';
      if (ok && !this.useLocalLlm && this.isOnline) {
        this.scheduleLocalLlmRelease();
      }
    });
  }

  // ========== 本地 LLM（Qwen2.5-0.5B GGUF + llama.cpp）==========

  private callLocalLlmStreaming(
    text: string,
    onToken: (token: string) => void,
    onComplete: (fullText: string) => void
  ): void {
    const historyJson: string = JSON.stringify(this.buildLlmPrelude(text));
    const callbacks: LocalLlmRuntimeCallbacks = {
      onStatus: (message: string, state: string): void => {
        if (message) {
          this.localLlmStatus = message;
          this.logInfo = message;
        }
        this.localLlmState = state;
      },
      appendContext: (userText: string, assistantText: string): void => {
        this.llmContext.append(userText, assistantText);
      },
      shouldReleaseAfterRequest: (): boolean => !this.useLocalLlm && this.isOnline,
      onReleased: (state: string): void => {
        this.localLlmState = state;
        this.localLlmStatus = '本地模型已安装（未加载）';
      }
    };
    this.localLlmController.generate(text, historyJson, onToken, onComplete, callbacks);
  }

  // ========== 本地 LLM 下载 ==========

  private refreshLocalModelDetails(): void {
    const info: LocalLlmModelInfo = this.localLlmController.getModelInfo();
    this.localModelFilePresent = info.filePresent;
    if (!info.filePresent) {
      this.localModelDetails = '模型文件未下载';
      return;
    }
    const sizeMb: string = (info.sizeBytes / 1024 / 1024).toFixed(1);
    const source: string = info.source ? ' · ' + info.source : '';
    const loaded: string = info.loaded ? ' · 已加载' : '';
    let verifiedAt: string = '';
    if (info.verifiedAt) {
      verifiedAt = ' · ' + info.verifiedAt.replace('T', ' ').substring(0, 16);
    }
    this.localModelDetails = sizeMb + ' MB' + source + ' · ' +
      info.integrity + loaded + verifiedAt;
  }

  private startLlmDownload(): void {
    if (this.isDownloading || this.isModelVerifying) return;
    this.downloadCancelledByUser = false;
    this.isDownloading = true;
    this.dlProgress = 0;
    // 异步启动下载（不阻塞 UI）
    this.localLlmController.downloadAndSetup(
      (pct: number, speed: number): void => {
        this.dlProgress = pct;
        this.dlSpeed = speed;
      },
      (msg: string): void => {
        this.localLlmStatus = msg;
        this.localLlmState = this.localLlmController.state;
      }
    ).then((ok: boolean): void => {
      this.isDownloading = false;
      this.localLlmState = this.localLlmController.state;
      this.refreshLocalModelDetails();
      if (ok) {
        this.manualLocalLlm = this.llmRoutingMode === ROUTING_LOCAL;
        this.useLocalLlm = this.llmRoutingMode === ROUTING_LOCAL;
        this.logInfo = '📡 本地模型已就绪';
      } else if (this.downloadCancelledByUser) {
        this.manualLocalLlm = this.llmRoutingMode === ROUTING_LOCAL;
        this.useLocalLlm = this.llmRoutingMode === ROUTING_LOCAL;
        this.localLlmStatus = '下载已取消，可稍后继续';
        this.logInfo = '本地模型下载已取消';
      } else {
        this.manualLocalLlm = this.llmRoutingMode === ROUTING_LOCAL;
        this.useLocalLlm = this.llmRoutingMode === ROUTING_LOCAL;
        this.logInfo = '❌ 本地模型安装失败';
      }
    });
  }

  private cancelLlmDownload(): void {
    if (!this.isDownloading) return;
    this.downloadCancelledByUser = true;
    this.localLlmController.cancelDownload();
    this.isDownloading = false;
    this.manualLocalLlm = this.llmRoutingMode === ROUTING_LOCAL;
    this.useLocalLlm = this.llmRoutingMode === ROUTING_LOCAL;
    this.localLlmState = this.localLlmController.state;
    this.localLlmStatus = '下载已取消，进度已保留';
    this.logInfo = '本地模型下载已取消，下次可断点续传';
    this.refreshLocalModelDetails();
  }

  private verifyLocalModel(): void {
    if (this.isDownloading || this.isModelVerifying) return;
    if (this.localLlmController.activeRequestCount > 0) {
      this.logInfo = '本地模型正在生成回复，请稍后校验';
      return;
    }
    this.cancelLocalLlmRelease();
    this.isModelVerifying = true;
    this.localLlmController.verifyInstalledModel((msg: string): void => {
      this.localLlmStatus = msg;
      this.localLlmState = this.localLlmController.state;
    }).then((ok: boolean): void => {
      this.isModelVerifying = false;
      this.localLlmState = this.localLlmController.state;
      this.refreshLocalModelDetails();
      this.logInfo = ok ? '本地模型完整性校验通过' : '本地模型校验失败，请重新下载';
    }).catch((e: Error): void => {
      this.isModelVerifying = false;
      this.localLlmState = this.localLlmController.state;
      this.refreshLocalModelDetails();
      this.logInfo = '本地模型校验失败: ' + String(e);
    });
  }

  private deleteLocalModel(): void {
    if (this.isDownloading || this.isModelVerifying) return;
    if (this.localLlmController.activeRequestCount > 0) {
      this.logInfo = '本地模型正在生成回复，请稍后删除';
      return;
    }
    AlertDialog.show({
      title: '删除本地模型',
      message: '将释放约 380–470 MB 存储空间。删除前会先卸载本地推理引擎，之后仍可重新下载。',
      autoCancel: true,
      primaryButton: {
        value: '取消',
        action: (): void => {}
      },
      secondaryButton: {
        value: '删除',
        action: (): void => {
          this.cancelLocalLlmRelease();
          const removed: boolean = this.localLlmController.deleteInstalledModel((msg: string): void => {
            this.localLlmStatus = msg;
          });
          this.manualLocalLlm = this.llmRoutingMode === ROUTING_LOCAL;
          this.useLocalLlm = this.llmRoutingMode === ROUTING_LOCAL;
          this.localLlmState = this.localLlmController.state;
          this.refreshLocalModelDetails();
          this.logInfo = removed ? '本地模型已删除：可重新下载或配置远端模型' : '本地模型删除失败';
        }
      }
    });
  }

  private redownloadLocalModel(): void {
    if (this.isDownloading || this.isModelVerifying) return;
    if (this.localLlmController.activeRequestCount > 0) {
      this.logInfo = '本地模型正在生成回复，请稍后重下载';
      return;
    }
    AlertDialog.show({
      title: '重新下载本地模型',
      message: '现有模型会先安全卸载并删除，然后重新下载和校验。',
      autoCancel: true,
      primaryButton: {
        value: '取消',
        action: (): void => {}
      },
      secondaryButton: {
        value: '重新下载',
        action: (): void => {
          this.cancelLocalLlmRelease();
          const removed: boolean = this.localLlmController.deleteInstalledModel();
          this.localLlmState = this.localLlmController.state;
          this.refreshLocalModelDetails();
          if (removed) {
            this.startLlmDownload();
          } else {
            this.logInfo = '现有模型删除失败，未开始重新下载';
          }
        }
      }
    });
  }

  // ========== 远端 LLM（HTTP API）==========

  // LLM 流式调用：纯本地 / 智能路由 / 指定远端三种显式策略。
  private getRemoteSourceLabel(model: ModelOption): string {
    return model.kind === 'ollama' ? '🏠 自建 Ollama' : '☁️ ' + model.label;
  }

  private callLlmStreaming(
    userText: string,
    onToken: (token: string) => void,
    onComplete: (fullText: string) => void,
    onRoute?: (source: string) => void
  ): void {
    this.llmRequestOrchestrator.execute({
      mode: this.llmRoutingMode,
      userText,
      localInstalled: this.localLlmController.isInstalled,
      online: this.isOnline,
      remoteConfigured: this.isRemoteConfigured(),
      remoteSourceLabel: this.getRemoteSourceLabel(MODEL_LIST[this.currentModelIdx]),
      remoteUrl: this.normalizedRemoteUrl(),
      remoteApiKey: this.remoteApiKey.trim(),
      remoteModelName: this.remoteModelName.trim()
    }, {
      runLocal: (
        text: string,
        tokenCallback: (token: string) => void,
        completeCallback: (fullText: string) => void
      ): void => {
        this.callLocalLlmStreaming(text, tokenCallback, completeCallback);
      },
      buildRemoteMessages: (text: string): LlmMessage[] => {
        const userMsg: LlmMessage = { role: 'user', content: text };
        return [...this.buildLlmPrelude(text, true), userMsg];
      },
      appendContext: (text: string, assistantText: string): void => {
        this.llmContext.append(text, assistantText);
      },
      scheduleLocalRelease: (): void => this.scheduleLocalLlmRelease(),
      captureMemory: (label: string): void => {
        RuntimeMemoryManager.capture(label);
      },
      updateRemoteState: (update: RemoteStateUpdate): void => {
        this.remoteConfigVerified = update.verified;
        this.remoteConfigStatus = update.configStatus;
        if (update.localStatus) this.localLlmStatus = update.localStatus;
      },
      updateLogInfo: (message: string): void => {
        this.logInfo = message;
      }
    }, {
      onToken,
      onComplete,
      onRoute
    });
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
    console.info('OHOS_ai_STARTTTS: textLen=' + text.length + ' worker=' +
      this.speechWorkers.hasTtsWorker + ' pending=' + this.ttsPendingCount);
    if (!text) return;
    this.cancelTtsIdleRelease();
    RuntimeMemoryManager.capture('TTS启动前');

    this.ttsPendingCount++;
    // TTS Worker 内部已按流水线处理多句，完成后发一次 'stopped'
    if (this.speechWorkers.startTts(text, this.prepareModelDir(), this.speed)) {
      if (this.fullDuplexActive) {
        nativeLib.notifyDuplexEvent('tts_started', '');
      }
    } else {
      this.ttsPendingCount = Math.max(0, this.ttsPendingCount - 1);
    }
  }

  // ---------- UI 交互 ----------
  // 停止 ASR 但不影响 isRecording 状态
  private stopAsrInternal(): void {
    this.speechWorkers.stopAsr();
  }

  private toggleRecord(): void {
    if (this.isRecording) {
      this.speechWorkers.stopAsr();
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

  private lastAssistantReply(): string {
    for (let i: number = this.chatList.length - 1; i >= 0; i--) {
      if (this.chatList[i].from === 'assistant' && this.chatList[i].msg.trim() &&
          !this.chatList[i].source?.startsWith('本地指令')) {
        return this.chatList[i].msg.trim();
      }
    }
    return '';
  }

  private addLocalSkillExchange(userText: string, response: string, includeUser: boolean = true): void {
    if (includeUser) {
      this.chatList.push({ msg: userText, from: 'user' });
      MetricsCollector.recordMessage('user');
    }
    if (response) {
      this.chatList.push({ msg: response, from: 'assistant', source: '本地指令 · 未调用模型' });
      MetricsCollector.recordMessage('assistant');
    }
    this.uiRefreshKey++;
    this.scrollToBottom();
    this.saveCurrentConversation();
  }

  private localRuntimeStatus(): string {
    const route: string = this.routingModeLabel().replace(/[🧭☁️📡]/g, '').trim();
    const network: string = this.isOnline ? '网络可用' : '网络不可用';
    let model: string = '本地模型未安装';
    if (this.localLlmController.isReady) model = '本地模型已加载';
    else if (this.localLlmController.isInstalled) model = '本地模型已安装，当前未加载';
    return '当前为' + route + '，' + network + '，' + model + '。隐私类内容在智能路由下优先留在本机。';
  }

  /**
   * Executes only high-confidence deterministic commands. Returns false for
   * normal conversation so the existing LLM path remains untouched.
   */
  private tryHandleLocalSkill(text: string, fromDuplex: boolean): boolean {
    const route: DeterministicSkillRoute = routeDeterministicSkill(text);
    if (route.domain === 'reminder') {
      console.info('OHOS_REMINDER: command=' + route.reminder.kind + ' text="' + sanitizeLog(text) + '"');
      this.executeLocalOperation(text, this.localSkillController.executeReminder(route.reminder),
        fromDuplex, 'reminder');
      return true;
    }
    if (route.domain === 'memo') {
      console.info('OHOS_MEMO: command=' + route.memo.kind + ' text="' + sanitizeLog(text) + '"');
      this.executeLocalOperation(text, this.localSkillController.executeMemo(route.memo),
        fromDuplex, 'memo');
      return true;
    }
    if (route.domain !== 'control') return false;
    console.info('OHOS_SKILL: matched=' + route.control.skill + ' text="' + sanitizeLog(text) + '"');
    const result: LocalControlResult = this.localSkillController.resolveControl(route.control, {
      previousAssistantReply: this.lastAssistantReply(),
      runtimeStatus: this.localRuntimeStatus(),
      speed: this.speed,
      localModelInstalled: this.localLlmController.isInstalled,
      remoteConfigured: this.isRemoteConfigured()
    });

    if (result.effect === 'stop_tts') {
      try {
        this.speechWorkers.sendTtsCommand('stopTts');
      } catch (e) {
        console.warn('OHOS_SKILL: stop TTS post failed=' + String(e));
      }
    } else if (result.effect === 'new_conversation') {
      this.newConversation();
    } else if (result.effect === 'clear_conversation') {
      this.clearConversation();
      this.llmContext.clear();
      this.llmInterruptedContext = '';
      this.lastFinalText = '';
    } else if (result.effect === 'route_local') {
      this.setRoutingMode(ROUTING_LOCAL);
    } else if (result.effect === 'route_smart') {
      this.setRoutingMode(ROUTING_SMART);
    } else if (result.effect === 'route_remote') {
      this.setRoutingMode(ROUTING_REMOTE);
    } else if (result.effect === 'open_remote_settings') {
      this.isModelPickerVisible = true;
    }

    this.speed = result.nextSpeed;
    this.inputText = '';
    this.logInfo = result.response;
    this.addLocalSkillExchange(text, result.response, result.includeUser);

    if (fromDuplex) {
      if (result.speechText) this.startTts(result.speechText);
      // Complete the LLM_WAITING handshake even though no model was called.
      nativeLib.notifyDuplexEvent('llm_complete', result.speechText);
    }
    return true;
  }

  private executeLocalOperation(text: string, operation: Promise<LocalOperationResult>,
    fromDuplex: boolean, domain: string): void {
    this.inputText = '';
    this.logInfo = domain === 'reminder' ? '正在处理提醒…' : '正在处理备忘…';
    operation.then((result: LocalOperationResult): void => {
      const response: string = result.message;
      this.logInfo = response;
      this.addLocalSkillExchange(text, response);
      if (domain === 'reminder') this.refreshReminderItems();
      else this.refreshMemoItems();
      if (fromDuplex) {
        if (response) this.startTts(response);
        nativeLib.notifyDuplexEvent('llm_complete', response);
      }
    }).catch((error: Object): void => {
      const response: string = domain === 'reminder' ? '提醒操作失败，请稍后再试。' :
        '备忘操作失败，请稍后再试。';
      console.error('OHOS_LOCAL_SKILL: ' + domain + ' failed=' + String(error));
      this.logInfo = response;
      this.addLocalSkillExchange(text, response);
      if (fromDuplex) {
        this.startTts(response);
        nativeLib.notifyDuplexEvent('llm_complete', response);
      }
    });
  }

  private onReminderTriggered(content: string): void {
    const response: string = '提醒时间到了：' + content + '。';
    console.info('OHOS_REMINDER: presenting in-app reminder content="' + sanitizeLog(content) + '"');
    this.logInfo = response;
    this.chatList.push({ msg: response, from: 'assistant', source: '本地提醒 · 未调用模型' });
    MetricsCollector.recordMessage('assistant');
    this.uiRefreshKey++;
    this.scrollToBottom();
    this.saveCurrentConversation();
    this.startTts(response);
    this.refreshReminderItems();
  }

  private refreshMemoItems(): void {
    this.memoItems = MemoService.getRecords();
  }

  private async deleteMemoFromPanel(id: number): Promise<void> {
    const result: MemoResult = await MemoService.deleteById(id);
    this.logInfo = result.message;
    this.refreshMemoItems();
  }

  private memoTimeLabel(timestamp: number): string {
    const date: Date = new Date(timestamp);
    const month: number = date.getMonth() + 1;
    const minute: string = date.getMinutes() < 10 ? '0' + date.getMinutes() : String(date.getMinutes());
    return month + '月' + date.getDate() + '日 ' + date.getHours() + ':' + minute;
  }

  private refreshReminderItems(): void {
    ReminderService.getRecords().then((items: LocalReminderRecord[]): void => {
      this.reminderItems = items;
    }).catch((e: Object): void => {
      console.warn('OHOS_REMINDER: refresh UI failed=' + String(e));
    });
  }

  private cancelReminderFromPanel(reminderId: number): void {
    ReminderService.cancelById(reminderId).then((result: ReminderResult): void => {
      this.logInfo = result.message;
      this.refreshReminderItems();
    }).catch((e: Object): void => {
      console.error('OHOS_REMINDER: panel cancel failed=' + String(e));
      this.logInfo = '取消提醒失败，请稍后再试。';
    });
  }

  private reminderTimeLabel(timestamp: number): string {
    const date: Date = new Date(timestamp);
    const now: Date = new Date();
    const tomorrow: Date = new Date(now.getFullYear(), now.getMonth(), now.getDate() + 1);
    let day: string = (date.getMonth() + 1) + '月' + date.getDate() + '日';
    if (date.toDateString() === now.toDateString()) day = '今天';
    else if (date.toDateString() === tomorrow.toDateString()) day = '明天';
    const hour: string = date.getHours() < 10 ? '0' + date.getHours() : String(date.getHours());
    const minute: string = date.getMinutes() < 10 ? '0' + date.getMinutes() : String(date.getMinutes());
    return day + ' ' + hour + ':' + minute;
  }

  private openPrivacyCenter(): void {
    this.isSidebarVisible = false;
    this.isPrivacyPanelVisible = true;
    this.privacyStatus = '';
    this.refreshPrivacyOverview();
  }

  private async refreshPrivacyOverview(): Promise<void> {
    try {
      const overview: PersonalDataOverview = await this.personalData().loadOverview();
      this.privacyFacts = overview.facts;
      this.privacyPreferences = overview.preferences;
      this.privacyTopics = overview.topics;
      this.privacyConversationCount = overview.conversationCount;
      this.memoItems = overview.memos;
      this.reminderItems = overview.reminders;
      this.privacyReminderCount = overview.reminders.length;
    } catch (e) {
      this.privacyStatus = '本地数据读取失败，请稍后重试。';
      console.error('OHOS_PRIVACY: refresh failed=' + String(e));
    }
  }

  private runPrivacyAudit(): void {
    if (this.privacyAuditRunning || this.privacyBusy) return;
    this.privacyAuditRunning = true;
    this.privacyAuditSummary = '正在检查本地存储…';
    try {
      const report: PrivacyAuditReport = this.personalData().runStorageAudit();
      this.privacyAuditItems = report.items;
      if (report.failureCount > 0) {
        this.privacyAuditSummary = '检查失败：发现 ' + report.failureCount + ' 项需要处理。';
      } else if (report.warningCount > 0) {
        this.privacyAuditSummary = '核心个人数据加密正常，另有 ' + report.warningCount + ' 项边界警告。';
      } else {
        this.privacyAuditSummary = '检查通过：' + report.encryptedFileCount + ' 个个人数据文件均为认证密文。';
      }
      console.info('OHOS_PRIVACY_AUDIT: encrypted=' + report.encryptedFileCount +
        ' warning=' + report.warningCount + ' failure=' + report.failureCount);
    } catch (e) {
      this.privacyAuditItems = [];
      this.privacyAuditSummary = '安全检查失败，请稍后重试。';
      console.error('OHOS_PRIVACY_AUDIT: run failed=' + String(e));
    } finally {
      this.privacyAuditRunning = false;
    }
  }

  private confirmDeleteMemory(kind: string, key: string, display: string): void {
    AlertDialog.show({
      title: '删除这条记忆？',
      message: '删除后，后续回答将不再使用“' + display + '”。',
      autoCancel: true,
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: {
        value: '删除',
        fontColor: '#D16B62',
        action: (): void => { this.deleteMemoryItem(kind, key); }
      }
    });
  }

  private async deleteMemoryItem(kind: string, key: string): Promise<void> {
    if (this.privacyBusy) return;
    this.privacyBusy = true;
    this.privacyStatus = '';
    try {
      const removed: boolean = await this.personalData().deleteMemory(kind as PersonalMemoryKind, key);
      this.privacyStatus = removed ? '记忆已删除，并已更新本地加密存储。' : '这条记忆已经不存在。';
      await this.refreshPrivacyOverview();
    } catch (e) {
      this.privacyStatus = '删除记忆失败，请稍后重试。';
      console.error('OHOS_PRIVACY: memory delete failed=' + String(e));
    } finally {
      this.privacyBusy = false;
    }
  }

  private confirmClearMemory(): void {
    AlertDialog.show({
      title: '清空长期记忆？',
      message: '将删除助手记住的事实、偏好、话题和对话计数，但保留聊天记录、备忘和提醒。',
      autoCancel: true,
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: {
        value: '确认清空',
        fontColor: '#D16B62',
        action: (): void => { this.clearLongTermMemory(); }
      }
    });
  }

  private async clearLongTermMemory(): Promise<void> {
    if (this.privacyBusy) return;
    this.privacyBusy = true;
    try {
      await this.personalData().clearLongTermMemory();
      this.privacyStatus = '长期记忆已清空，当前聊天记录仍然保留。';
      await this.refreshPrivacyOverview();
    } catch (e) {
      this.privacyStatus = '清空长期记忆失败，请稍后重试。';
      console.error('OHOS_PRIVACY: memory clear failed=' + String(e));
    } finally {
      this.privacyBusy = false;
    }
  }

  private confirmClearPersonalData(): void {
    AlertDialog.show({
      title: '清除全部个人内容？',
      message: '将取消全部提醒，并删除长期记忆、聊天记录和备忘。模型文件与远端连接配置不会删除。此操作不可撤销。',
      autoCancel: true,
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: {
        value: '全部清除',
        fontColor: '#D16B62',
        action: (): void => { this.clearAllPersonalData(); }
      }
    });
  }

  private async clearAllPersonalData(): Promise<void> {
    if (this.privacyBusy) return;
    if (this.localLlmController.activeRequestCount > 0 || this.fullDuplexState === 'LLM_WAITING' ||
        this.logInfo.indexOf('AI 思考中') >= 0) {
      this.privacyStatus = '请等待当前回答完成后再清除个人内容。';
      return;
    }
    this.privacyBusy = true;
    this.privacyStatus = '正在清除本地个人内容…';
    try {
      await this.personalData().clearAllPersonalData();

      this.conversationController.invalidateRestore();
      this.historyList = [];
      this.chatList = [];
      this.conversationController.startNewConversation();
      this.activeHistoryId = '';
      this.llmContext.clear();
      this.llmInterruptedContext = '';
      this.lastFinalText = '';
      this.inputText = '';
      this.memoItems = [];
      this.reminderItems = [];
      this.privacyReminderCount = 0;
      this.uiRefreshKey++;
      this.privacyStatus = '个人内容已清除；模型文件和连接配置仍然保留。';
      await this.refreshPrivacyOverview();
      console.info('OHOS_PRIVACY: all personal content cleared');
    } catch (e) {
      this.privacyStatus = '清除未全部完成：' + String(e);
      console.error('OHOS_PRIVACY: clear all failed=' + String(e));
      await this.refreshPrivacyOverview();
    } finally {
      this.privacyBusy = false;
    }
  }

  private clearConversation(): void {
    this.chatList = [];
    this.inputText = '';
    this.scrollToBottom();
  }

  private scrollToBottom(): void {
    // Streaming output can request scrolling many times per second. Keep only
    // the latest request so timers do not accumulate during long replies.
    if (this.scrollTimer >= 0) {
      clearTimeout(this.scrollTimer);
    }
    this.scrollTimer = setTimeout((): void => {
      this.scrollTimer = -1;
      this.scrollRef.scrollEdge(Edge.Bottom);
    }, 100);
  }

  private sendTextMsg(): void {
    const text: string = this.inputText.trim();
    console.info('OHOS_FLOW: sendTextMsg text="' + sanitizeLog(text.substring(0, 50)) + '" fullDuplex=' + this.fullDuplexActive);
    if (!text) return;
    if (this.tryHandleLocalSkill(text, false)) return;
    // 更新会话状态
    const activeConversationId: string = this.conversationController.ensureCurrentConversationId();
    MemoryManager.touchSession(activeConversationId);
    this.saveCurrentConversation();
    const userMsg: ChatItem = { msg: text, from: 'user' };
    this.chatList.push(userMsg);
    MetricsCollector.recordMessage('user');
    this.inputText = '';
    this.scrollToBottom();
    this.logInfo = 'AI 思考中…';
    // 记录当前对话 ID，用于回调时校验是否还是同一个对话
    const reqConvId: string = this.conversationController.currentConversationId;
    const llmStartTime: number = Date.now();
    // 插入空的 assistant 消息占位，后续 onToken 逐步填充内容
    const aiMsg: ChatItem = { msg: '', from: 'assistant' };
    this.chatList.push(aiMsg);
    const aiMessageIndex: number = this.chatList.length - 1;
    this.scrollToBottom();
    const llmReqStart: number = Date.now();
    this.callLlmStreaming(text,
      (token: string): void => {
        if (!this.conversationController.isCurrent(reqConvId)) return;
        const idx: number = aiMessageIndex;
        if (idx >= 0 && this.chatList[idx].from === 'assistant') {
          const updated: ChatItem = {
            msg: this.chatList[idx].msg + token,
            from: 'assistant',
            source: this.chatList[idx].source,
          };
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
        if (!this.conversationController.isCurrent(reqConvId)) {
          console.info('OHOS_Native_ai_engine: discarding LLM streaming reply (switched conversation)');
          return;
        }
        // 如果最终回复为空（出错），设置占位消息为错误提示
        if (!fullText) {
          const idx: number = aiMessageIndex;
          if (idx >= 0 && this.chatList[idx].from === 'assistant' && !this.chatList[idx].msg) {
            this.chatList[idx].msg = '抱歉，AI 返回了空响应。';
            this.chatList = [...this.chatList];
          }
        } else {
          const idx: number = aiMessageIndex;
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
          // 整段文本一次性送 TTS，不分句拆分。
          // sherpa-onnx TTS 引擎内部会处理句子边界，流式回调自然衔接无间隙
          if (fullText) {
            this.startTts(fullText);
          }
          nativeLib.notifyDuplexEvent('llm_complete', fullText || '');
        }
      },
      (source: string): void => {
        if (!this.conversationController.isCurrent(reqConvId)) return;
        if (aiMessageIndex >= 0 && aiMessageIndex < this.chatList.length &&
            this.chatList[aiMessageIndex].from === 'assistant') {
          this.chatList[aiMessageIndex].source = source;
          this.chatList = [...this.chatList];
          this.uiRefreshKey++;
        }
      }
    );
  }

  // ========== 状态机 Action 回调 ==========

  // 由 setDuplexCallbacks → 'llm_request' 触发，状态机要求调用 LLM
  private onLlmRequested(text: string): void {
    if (!text || !this.fullDuplexActive) return;
    console.info('OHOS_FLOW: onLlmRequested text="' + sanitizeLog(text.substring(0, 50)) + '"');
    if (this.tryHandleLocalSkill(text, true)) return;
    this.conversationController.ensureCurrentConversationId();
    const reqConvId: string = this.conversationController.currentConversationId;
    const llmStartTime: number = Date.now();
    // TTS 在完整回复生成后才开始，上一条 assistant 回复已在上下文控制器中。
    // 打断后继续使用原始 ASR 文本，不能再次把旧回复伪装成 user 内容。
    const promptText: string = text;
    this.llmInterruptedContext = '';

    // 插入用户消息
    const userMsg: ChatItem = { msg: promptText, from: 'user' };
    this.chatList.push(userMsg);
    this.scrollToBottom();
    // 插入空的 assistant 占位
    const aiMsg: ChatItem = { msg: '', from: 'assistant' };
    this.chatList.push(aiMsg);
    const aiMessageIndex: number = this.chatList.length - 1;
    this.scrollToBottom();
    this.logInfo = '🤖 AI 思考中…';

    this.callLlmStreaming(promptText,
      (token: string): void => {
        if (!this.conversationController.isCurrent(reqConvId)) return;
        const idx: number = aiMessageIndex;
        if (idx >= 0 && this.chatList[idx].from === 'assistant') {
          const updated: ChatItem = {
            msg: this.chatList[idx].msg + token,
            from: 'assistant',
            source: this.chatList[idx].source,
          };
          this.chatList[idx] = updated;
          this.uiRefreshKey++;
        }
        this.scrollToBottom();
      },
      (fullText: string): void => {
        MetricsCollector.recordLlmLatency(Date.now() - llmStartTime);
        if (!this.conversationController.isCurrent(reqConvId)) return;
        if (!fullText) {
          const idx: number = aiMessageIndex;
          if (idx >= 0 && this.chatList[idx].from === 'assistant' && !this.chatList[idx].msg) {
            this.chatList[idx].msg = '抱歉，AI 返回了空响应。';
            this.chatList = [...this.chatList];
          }
        } else {
          const idx: number = aiMessageIndex;
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
        // 整段文本一次性送 TTS，不分句（避免句间停顿）
        if (fullText) this.startTts(fullText);
        nativeLib.notifyDuplexEvent('llm_complete', fullText || '');
      },
      (source: string): void => {
        if (!this.conversationController.isCurrent(reqConvId)) return;
        if (aiMessageIndex >= 0 && aiMessageIndex < this.chatList.length &&
            this.chatList[aiMessageIndex].from === 'assistant') {
          this.chatList[aiMessageIndex].source = source;
          this.chatList = [...this.chatList];
          this.uiRefreshKey++;
        }
      }
    );
  }

  // ========== 历史记录管理 ==========

  private saveCurrentConversation(): void {
    const items: ConversationChatItem[] = this.chatList;
    this.conversationController.save(items).then((): void => {
      console.info('OHOS_Native_ai_engine: history saved, id=' +
        this.conversationController.currentConversationId);
      this.loadHistoryList();
    }).catch((e: Error): void => {
      console.error('OHOS_Native_ai_engine: save history failed: ' + String(e));
    });
  }

  private loadHistoryList(): void {
    this.conversationController.list().then((sessions: ConversationSessionItem[]): void => {
      this.historyList = sessions;
    }).catch((e: Error): void => {
      console.error('OHOS_HISTORY: encrypted list failed=' + String(e));
    });
  }

  private restoreConversation(sessionId: string): void {
    console.info('OHOS_Native_ai_engine: restore id=' + sessionId);
    this.activeHistoryId = sessionId;
    if (this.scrollTimer >= 0) {
      clearTimeout(this.scrollTimer);
      this.scrollTimer = -1;
    }
    // 不在异步解密期间继续展示上一个会话，避免用户误以为已切换成功。
    this.chatList = [];
    this.llmContext.clear();
    this.inputText = '';
    this.lastFinalText = '';
    this.logInfo = '正在加载历史对话…';
    this.uiRefreshKey++;
    this.conversationController.restore(sessionId).then((result: ConversationRestoreResult | null): void => {
      if (!result || this.activeHistoryId !== sessionId) {
        console.info('OHOS_HISTORY: discarded stale restore id=' + sessionId);
        return;
      }
      console.info('OHOS_Native_ai_engine: restore got ' + result.items.length);
      if (result.items.length > 0) {
        this.chatList = result.items;
        this.uiRefreshKey++;
        this.inputText = '';
        this.rebuildLlmHistoryFromChat();
        this.lastFinalText = '';
        this.logInfo = '';
        // 延迟一帧滚动到底部
        setTimeout((): void => {
          if (!this.conversationController.isRestoreCurrent(result.token, sessionId)) return;
          this.scrollRef.scrollEdge(Edge.Bottom);
        }, 100);
      } else {
        this.logInfo = '历史对话不存在或无法读取';
      }
    }).catch((e: Error): void => {
      if (!this.conversationController.isCurrent(sessionId)) return;
      this.logInfo = '历史对话加载失败';
      console.error('OHOS_Native_ai_engine: restore failed for ' + sessionId);
    });
  }

  private deleteHistory(sessionId: string): void {
    this.conversationController.delete(sessionId).then((deletedActive: boolean): void => {
      this.loadHistoryList();
      if (deletedActive || this.activeHistoryId === sessionId) {
        this.conversationController.startNewConversation();
        this.activeHistoryId = '';
        this.chatList = [];
        this.inputText = '';
        this.llmContext.clear();
        this.lastFinalText = '';
        this.logInfo = '对话已删除';
      }
    }).catch((e: Error): void => {
      console.error('OHOS_HISTORY: delete failed=' + String(e));
    });
  }

  private clearAllHistory(): void {
    this.conversationController.clear().then((): void => {
      this.historyList = [];
      this.chatList = [];
      this.llmContext.clear();
      this.saveCurrentConversation();
    }).catch((e: Error): void => {
      console.error('OHOS_HISTORY: clear failed=' + String(e));
    });
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
    this.conversationController.startNewConversation();
    this.chatList = [];
    this.inputText = '';
    this.llmContext.clear();
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



  private prepareModelDir(): string {
    return getContext().filesDir + '/models/tts';
  }

  // ---------- 语音坞展示映射（不改变 Native 状态机） ----------
  private voiceStateLabel(): string {
    if (!this.fullDuplexActive) return this.isRecording ? '正在录音' : '准备就绪';
    switch (this.fullDuplexState) {
      case 'LISTENING': return this.asrInterimText ? '正在识别' : '正在聆听';
      case 'ASR_PROCESSING': return '正在识别';
      case 'LLM_WAITING': return '正在思考';
      case 'TTS_PLAYING': return '正在朗读';
      case 'BARGE_IN_CANDIDATE': return '听到你了';
      case 'INTERRUPTED': return '已停止朗读';
      default: return '准备就绪';
    }
  }

  private voiceStateHint(): string {
    if (!this.fullDuplexActive) {
      return this.isRecording ? '说完后点击停止' : '点击麦克风开始说话';
    }
    switch (this.fullDuplexState) {
      case 'LISTENING': return this.asrInterimText ? '正在把你的话变成文字' : '直接说话即可';
      case 'ASR_PROCESSING': return '正在确认这句话';
      case 'LLM_WAITING': return '正在组织回答';
      case 'TTS_PLAYING': return '说话可以随时打断';
      case 'BARGE_IN_CANDIDATE': return '正在确认是否需要打断';
      case 'INTERRUPTED': return '请继续说话';
      default: return '连续对话已开启';
    }
  }

  private voiceStateColor(): string {
    if (this.fullDuplexState === 'TTS_PLAYING') return '#3D8FC4';
    if (this.fullDuplexState === 'LLM_WAITING' || this.fullDuplexState === 'ASR_PROCESSING') return '#B57A31';
    if (this.fullDuplexState === 'INTERRUPTED') return '#D16B62';
    return PRIMARY_COLOR;
  }

  private voiceTranscript(): string {
    if (this.asrInterimText.trim()) return this.asrInterimText.trim();
    if (this.fullDuplexState === 'LLM_WAITING') return '已经听清了，正在准备回答…';
    if (this.fullDuplexState === 'TTS_PLAYING') return '正在朗读回复，你可以直接开口打断';
    if (this.fullDuplexState === 'BARGE_IN_CANDIDATE') return '我听到你在说话…';
    return '你说的话会实时显示在这里';
  }

  private shouldShowVoiceDock(): boolean {
    return this.fullDuplexActive || this.isRecording || this.asrInterimText.length > 0;
  }

  private shouldShowNotice(): boolean {
    if (!this.logInfo || this.shouldShowVoiceDock()) return false;
    return this.logInfo.includes('失败') || this.logInfo.includes('拒绝') ||
      this.logInfo.includes('断开') || this.logInfo.includes('需要') ||
      this.logInfo.includes('未下载') || this.logInfo.includes('已切换');
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
                source: item.source || '',
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
        // 遮罩与面板必须叠放。此前使用 Row 时遮罩先占满 100% 宽度，
        // ModelPicker 被布局到屏幕右侧，看起来像点击后出现空白弹层。
        Stack({ alignContent: Alignment.Center }) {
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

      // ===== 提醒列表覆盖层 =====
      if (this.isReminderPanelVisible) {
        Stack({ alignContent: Alignment.Center }) {
          Column()
            .width('100%').height('100%')
            .backgroundColor('rgba(0,0,0,0.36)')
            .onClick((): void => { this.isReminderPanelVisible = false; })
          this.ReminderPanel()
        }
        .width('100%').height('100%')
      }

      // ===== 备忘录覆盖层 =====
      if (this.isMemoPanelVisible) {
        Stack({ alignContent: Alignment.Center }) {
          Column()
            .width('100%').height('100%')
            .backgroundColor('rgba(0,0,0,0.36)')
            .onClick((): void => { this.isMemoPanelVisible = false; })
          this.MemoPanel()
        }
        .width('100%').height('100%')
      }

      // ===== 隐私与记忆管理中心 =====
      if (this.isPrivacyPanelVisible) {
        Stack({ alignContent: Alignment.Center }) {
          Column()
            .width('100%').height('100%')
            .backgroundColor('rgba(0,0,0,0.40)')
            .onClick((): void => { if (!this.privacyBusy) this.isPrivacyPanelVisible = false; })
          this.PrivacyCenterPanel()
        }
        .width('100%').height('100%')
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

      Row() {
        Text('◈').fontSize(18).fontColor(PRIMARY_DARK)
        Column() {
          Text('隐私与记忆').fontSize(14).fontColor('#334A45').fontWeight('600')
          Text('查看、纠正或清除本地个人数据').fontSize(10).fontColor('#8A9A96').margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start).margin({ left: 10 })
        Blank()
        Text('›').fontSize(20).fontColor('#8FB0A8')
      }
      .width('90%').padding(12).margin({ top: 10 })
      .backgroundColor('#EEF8F5').borderRadius(12)
      .onClick((): void => this.openPrivacyCenter())

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
      Column() {
        Text(this.routingModeLabel()).fontSize(13).fontColor('#6B7280').fontWeight('500')
          .width('100%')
        Row({ space: 5 }) {
          Button('本地')
            .fontSize(11).height(30).layoutWeight(1)
            .fontColor(this.llmRoutingMode === ROUTING_LOCAL ? '#FFFFFF' : '#6B7280')
            .backgroundColor(this.llmRoutingMode === ROUTING_LOCAL ? PRIMARY_COLOR : '#F3F4F6')
            .onClick((): void => this.setRoutingMode(ROUTING_LOCAL))
          Button('智能')
            .fontSize(11).height(30).layoutWeight(1)
            .fontColor(this.llmRoutingMode === ROUTING_SMART ? '#FFFFFF' : '#6B7280')
            .backgroundColor(this.llmRoutingMode === ROUTING_SMART ? PRIMARY_COLOR : '#F3F4F6')
            .onClick((): void => this.setRoutingMode(ROUTING_SMART))
          Button('远端')
            .fontSize(11).height(30).layoutWeight(1)
            .fontColor(this.llmRoutingMode === ROUTING_REMOTE ? '#FFFFFF' : '#6B7280')
            .backgroundColor(this.llmRoutingMode === ROUTING_REMOTE ? PRIMARY_COLOR : '#F3F4F6')
            .onClick((): void => this.setRoutingMode(ROUTING_REMOTE))
        }
        .width('100%').margin({ top: 6 })
      }
      .width('90%').alignItems(HorizontalAlign.Start)
      .padding({ top: 4, bottom: 4 })
      // 本地 LLM 状态与管理
      if (!this.localModelFilePresent && !this.isDownloading) {
        Row() {
          Text(this.localLlmStatus || '📡 本地模型未安装').fontSize(11).fontColor('#9CA3AF')
          Blank()
          Text('下载').fontSize(12).fontColor('#6366F1').fontWeight('600')
            .onClick((): void => this.startLlmDownload())
        }
        .width('90%')
        .padding({ top: 2, bottom: 2 })
      }
      if (this.localModelFilePresent && !this.isDownloading) {
        Column() {
          Text(this.localLlmStatus).fontSize(11)
            .fontColor(this.localLlmState === 'ERROR' ? '#EF4444' : '#6B7280')
            .width('100%')
          Text(this.localModelDetails).fontSize(10).fontColor('#9CA3AF')
            .width('100%').margin({ top: 2 })
          Row({ space: 16 }) {
            Text(this.isModelVerifying ? '校验中…' : '校验')
              .fontSize(11).fontColor(this.isModelVerifying ? '#9CA3AF' : '#6366F1')
              .onClick((): void => this.verifyLocalModel())
            Text('重新下载').fontSize(11).fontColor('#6366F1')
              .onClick((): void => this.redownloadLocalModel())
            Text('删除').fontSize(11).fontColor('#EF4444')
              .onClick((): void => this.deleteLocalModel())
          }
          .width('100%')
          .margin({ top: 5 })
        }
        .width('90%')
        .padding({ top: 3, bottom: 3 })
      }
      // 下载进度条
      if (this.isDownloading) {
        Column() {
          Row() {
            Text(this.localLlmStatus || '下载中…').fontSize(11).fontColor('#F59E0B')
            Blank()
            Text('取消').fontSize(11).fontColor('#EF4444')
              .onClick((): void => this.cancelLlmDownload())
          }
          .width('100%')
          Progress({ value: this.dlProgress, total: 100 }).width('100%').height(4).color('#6366F1')
          Text(this.dlProgress.toFixed(0) + '% (' + this.dlSpeed.toFixed(0) + ' KB/s)')
            .fontSize(10).fontColor('#9CA3AF')
        }
        .width('90%')
        .padding({ top: 4, bottom: 4 })
      }
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

      Row() {
        Column() {
          Text('智能路由（推荐）').fontSize(15).fontColor('#1F2937')
            .fontWeight(this.llmRoutingMode === ROUTING_SMART ? 700 : 400)
          Text('隐私和普通任务本地处理，事实问题按能力边界处理')
            .fontSize(11).fontColor('#9CA3AF').margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)
        Blank()
        if (this.llmRoutingMode === ROUTING_SMART) {
          Text('✓').fontSize(18).fontColor(PRIMARY_COLOR).fontWeight('700')
        }
      }
      .width('100%').padding(14)
      .backgroundColor(this.llmRoutingMode === ROUTING_SMART ? PRIMARY_LIGHT : '#FFFFFF')
      .borderRadius(10)
      .onClick((): void => {
        this.setRoutingMode(ROUTING_SMART);
        this.isModelPickerVisible = false;
      })

      Row() {
        Row() {
          Column() {
            Text('本机 GGUF（纯本地）').fontSize(15).fontColor('#1F2937')
              .fontWeight(this.llmRoutingMode === ROUTING_LOCAL ? 700 : 400)
            Text('下载后无需网络，模型仅在本机运行').fontSize(11).fontColor('#9CA3AF').margin({ top: 2 })
          }
          .alignItems(HorizontalAlign.Start)
          Blank()
          if (this.llmRoutingMode === ROUTING_LOCAL) {
            Text('✓').fontSize(18).fontColor(PRIMARY_COLOR).fontWeight('700')
          }
        }
        .width('100%').padding(14)
        .backgroundColor(this.llmRoutingMode === ROUTING_LOCAL ? PRIMARY_LIGHT : '#FFFFFF').borderRadius(10)
      }
      .width('100%')
      .onClick((): void => {
        this.setRoutingMode(ROUTING_LOCAL);
        this.isModelPickerVisible = false;
      })

      // 模型列表
      List({ space: 8 }) {
        ForEach(MODEL_LIST, (model: ModelOption, idx: number) => {
          ListItem() {
            Row() {
              Column() {
                Text(model.label)
                  .fontSize(15)
                  .fontColor('#1F2937')
                  .fontWeight(this.llmRoutingMode === ROUTING_REMOTE && idx === this.currentModelIdx ? 700 : 400)
                Text(model.modelName)
                  .fontSize(11).fontColor('#9CA3AF').margin({ top: 2 })
              }
              .alignItems(HorizontalAlign.Start)
              Blank()
              if (this.llmRoutingMode === ROUTING_REMOTE && idx === this.currentModelIdx) {
                Text('✓').fontSize(18).fontColor(PRIMARY_COLOR).fontWeight('700')
              }
            }
            .width('100%')
            .padding(14)
            .backgroundColor(this.llmRoutingMode === ROUTING_REMOTE && idx === this.currentModelIdx ? PRIMARY_LIGHT : '#FFFFFF')
            .borderRadius(10)
          }
          .onClick((): void => {
            this.selectRemoteProvider(idx);
          })
        }, (model: ModelOption, idx: number): string => idx.toString())
      }
      .width('100%')

      Divider().width('100%').margin({ top: 12, bottom: 8 })
      Text('远端连接配置（用户自带，不随应用提供）').fontSize(12).fontColor('#6B7280')
      TextInput({ text: this.remoteApiUrl, placeholder: this.isOllamaSelected() ?
        '可只填 192.168.x.x（自动补全端口和路径）' : 'OpenAI 兼容的 /v1/chat/completions 地址' })
        .width('100%').height(40).margin({ top: 6 })
        .onChange((value: string): void => { this.remoteApiUrl = value; this.remoteConfigVerified = false; })
      TextInput({ text: this.remoteModelName, placeholder: '模型名称，例如 qwen2.5:7b' })
        .width('100%').height(40).margin({ top: 6 })
        .onChange((value: string): void => { this.remoteModelName = value; this.remoteConfigVerified = false; })
      if (!this.isOllamaSelected()) {
        TextInput({ text: this.remoteApiKey, placeholder: '请输入您自己的 API Key' })
          .type(InputType.Password)
          .width('100%').height(40).margin({ top: 6 })
          .onChange((value: string): void => { this.remoteApiKey = value; this.remoteConfigVerified = false; })
      }
      Text(this.remoteConfigStatus).fontSize(11)
        .fontColor(this.remoteConfigVerified ? '#10B981' : '#9CA3AF').margin({ top: 5 })
      Button() {
        Text('保存并使用远端配置').fontSize(13).fontColor('#FFFFFF').fontWeight('600')
      }
      .width('100%').height(40).margin({ top: 8 })
      .backgroundColor(PRIMARY_COLOR).borderRadius(8)
      .onClick((): void => {
        if (!this.isRemoteConfigured()) {
          this.saveRemoteLlmConfig();
          return;
        }
        this.saveRemoteLlmConfig();
        this.setRoutingMode(ROUTING_REMOTE);
        this.isModelPickerVisible = false;
        this.logInfo = '已保存并切换远端，首次请求会验证连接';
      })
    }
    .width(300)
    .height('85%')
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
      Text('☰')
        .fontSize(20)
        .fontColor('#506560')
        .width(38)
        .height(38)
        .textAlign(TextAlign.Center)
        .backgroundColor('#F1F7F5')
        .borderRadius(19)
        .hoverEffect(HoverEffect.Highlight)
        .onClick((): void => { this.isSidebarVisible = !this.isSidebarVisible; })

      Column() {
        Text('本地语音助手')
          .fontSize(18).fontWeight('600').fontColor('#20332F')
        Row({ space: 5 }) {
          Circle().width(6).height(6).fill(this.fullDuplexActive ? '#45AFA8' : '#A7B8B3')
          Text(this.fullDuplexActive ? '持续聆听中' : '隐私优先 · 端侧可用')
            .fontSize(10).fontColor('#81928D')
        }
        .margin({ top: 2 })
      }
      .alignItems(HorizontalAlign.Start)
      .margin({ left: 12 })

      Blank()

      Row({ space: 6 }) {
        Stack({ alignContent: Alignment.TopEnd }) {
          Text('▤')
            .fontSize(17).fontColor('#397D77')
            .width(32).height(32).textAlign(TextAlign.Center)
            .backgroundColor('#F1F7F5').borderRadius(16)
          if (this.memoItems.length > 0) {
            Text(this.memoItems.length > 9 ? '9+' : this.memoItems.length.toString())
              .fontSize(8).fontColor('#FFFFFF').fontWeight('700')
              .textAlign(TextAlign.Center)
              .width(15).height(15).borderRadius(8)
              .backgroundColor('#6D9C92')
          }
        }
        .width(34).height(34)
        .onClick((): void => {
          this.refreshMemoItems();
          this.isMemoPanelVisible = true;
        })
        Stack({ alignContent: Alignment.TopEnd }) {
          Text('◷')
            .fontSize(18).fontColor('#397D77')
            .width(32).height(32).textAlign(TextAlign.Center)
            .backgroundColor('#F1F7F5').borderRadius(16)
          if (this.reminderItems.length > 0) {
            Text(this.reminderItems.length > 9 ? '9+' : this.reminderItems.length.toString())
              .fontSize(8).fontColor('#FFFFFF').fontWeight('700')
              .textAlign(TextAlign.Center)
              .width(15).height(15).borderRadius(8)
              .backgroundColor('#D96C62')
          }
        }
        .width(34).height(34)
        .onClick((): void => {
          this.refreshReminderItems();
          this.isReminderPanelVisible = true;
        })
        if (this.activeHistoryId) {
          Text('🗑')
            .fontSize(13)
            .width(30).height(30)
            .textAlign(TextAlign.Center)
            .backgroundColor('#FFF3F1')
            .borderRadius(15)
            .onClick((): void => {
              this.deleteHistory(this.activeHistoryId);
              this.logInfo = '已删除当前对话';
            })
        }
        Row({ space: 5 }) {
          Text('◇').fontSize(11).fontColor(PRIMARY_DARK)
          Text(this.remoteDisplayLabel()).fontSize(11).fontColor('#397D77').maxLines(1)
          Text('⌄').fontSize(10).fontColor('#7FA39D')
        }
        .backgroundColor(PRIMARY_LIGHT)
        .borderRadius(14)
        .padding({ left: 9, right: 9, top: 6, bottom: 6 })
        .onClick((): void => { this.isModelPickerVisible = true; })
      }
      .alignItems(VerticalAlign.Center)
    }
    .width('100%')
    .height(NAV_HEIGHT)
    .padding({ left: 14, right: 14 })
    .backgroundColor('rgba(255,255,255,0.96)')
    .border({ width: { bottom: 1 }, color: '#EAF0ED' })
  }

  // ===== 子组件：提醒列表 =====
  @Builder
  ReminderPanel(): void {
    Column() {
      Row() {
        Column() {
          Text('我的提醒').fontSize(18).fontWeight('600').fontColor('#20332F')
          Text('共 ' + this.reminderItems.length + ' 条待执行')
            .fontSize(11).fontColor('#81928D').margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)
        Blank()
        Text('✕')
          .fontSize(18).fontColor('#7F918C')
          .width(32).height(32).textAlign(TextAlign.Center)
          .backgroundColor('#F1F7F5').borderRadius(16)
          .onClick((): void => { this.isReminderPanelVisible = false; })
      }
      .width('100%').alignItems(VerticalAlign.Center).margin({ bottom: 14 })

      if (this.reminderItems.length === 0) {
        Column() {
          Text('◷').fontSize(36).fontColor('#A8C5BE')
          Text('还没有待执行提醒').fontSize(15).fontColor('#506560').margin({ top: 10 })
          Text('试着说“十分钟后提醒我喝水”')
            .fontSize(12).fontColor('#94A59F').margin({ top: 6 })
        }
        .width('100%').layoutWeight(1)
        .justifyContent(FlexAlign.Center)
      } else {
        List({ space: 10 }) {
          ForEach(this.reminderItems, (item: LocalReminderRecord) => {
            ListItem() {
              Row() {
                Column() {
                  Text(item.content).fontSize(15).fontWeight('500').fontColor('#253A35').maxLines(2)
                  Row({ space: 7 }) {
                    Text(this.reminderTimeLabel(item.triggerAtMs)).fontSize(11).fontColor('#6F827D')
                    Text(item.scheduler === 'system' ? '系统调度' : '应用内调度')
                      .fontSize(9)
                      .fontColor(item.scheduler === 'system' ? '#397D77' : '#A36A2E')
                      .backgroundColor(item.scheduler === 'system' ? '#E8F6F2' : '#FFF3DF')
                      .borderRadius(8).padding({ left: 6, right: 6, top: 2, bottom: 2 })
                  }
                  .margin({ top: 6 })
                }
                .alignItems(HorizontalAlign.Start).layoutWeight(1)
                Text('取消')
                  .fontSize(12).fontColor('#D16B62')
                  .padding({ left: 10, right: 10, top: 7, bottom: 7 })
                  .backgroundColor('#FFF2F0').borderRadius(12)
                  .onClick((): void => this.cancelReminderFromPanel(item.reminderId))
              }
              .width('100%').padding(14).alignItems(VerticalAlign.Center)
              .backgroundColor('#F8FBFA').borderRadius(14)
              .border({ width: 1, color: '#E5EFEC' })
            }
          }, (item: LocalReminderRecord): string => item.reminderId.toString())
        }
        .width('100%').layoutWeight(1)
      }
    }
    .width('88%').height('62%')
    .backgroundColor('#FFFFFF').borderRadius(22).padding(20)
    .shadow({ radius: 20, offsetX: 0, offsetY: 8, color: 'rgba(31,58,51,0.20)' })
  }

  // ===== 子组件：离线备忘录 =====
  @Builder
  MemoPanel(): void {
    Column() {
      Row() {
        Column() {
          Text('离线备忘').fontSize(18).fontWeight('600').fontColor('#20332F')
          Text('仅保存在本机 · 共 ' + this.memoItems.length + ' 条')
            .fontSize(11).fontColor('#81928D').margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)
        Blank()
        Text('✕')
          .fontSize(18).fontColor('#7F918C')
          .width(32).height(32).textAlign(TextAlign.Center)
          .backgroundColor('#F1F7F5').borderRadius(16)
          .onClick((): void => { this.isMemoPanelVisible = false; })
      }
      .width('100%').alignItems(VerticalAlign.Center).margin({ bottom: 14 })

      if (this.memoItems.length === 0) {
        Column() {
          Text('▤').fontSize(34).fontColor('#A8C5BE')
          Text('还没有备忘').fontSize(15).fontColor('#506560').margin({ top: 10 })
          Text('试着说“记一下周五交报告”')
            .fontSize(12).fontColor('#94A59F').margin({ top: 6 })
        }
        .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
      } else {
        List({ space: 10 }) {
          ForEach(this.memoItems, (item: MemoRecord) => {
            ListItem() {
              Row() {
                Column() {
                  Text(item.content).fontSize(15).fontWeight('500').fontColor('#253A35').maxLines(3)
                  Text(this.memoTimeLabel(item.createdAtMs))
                    .fontSize(10).fontColor('#8A9A96').margin({ top: 6 })
                }
                .alignItems(HorizontalAlign.Start).layoutWeight(1)
                Text('删除')
                  .fontSize(12).fontColor('#D16B62')
                  .padding({ left: 10, right: 10, top: 7, bottom: 7 })
                  .backgroundColor('#FFF2F0').borderRadius(12)
                  .onClick((): void => { this.deleteMemoFromPanel(item.id); })
              }
              .width('100%').padding(14).alignItems(VerticalAlign.Center)
              .backgroundColor('#F8FBFA').borderRadius(14)
              .border({ width: 1, color: '#E5EFEC' })
            }
          }, (item: MemoRecord): string => item.id.toString())
        }
        .width('100%').layoutWeight(1)
      }
    }
    .width('88%').height('62%')
    .backgroundColor('#FFFFFF').borderRadius(22).padding(20)
    .shadow({ radius: 20, offsetX: 0, offsetY: 8, color: 'rgba(31,58,51,0.20)' })
  }

  @Builder
  PrivacyCountCard(label: string, count: number): void {
    Column() {
      Text(count.toString()).fontSize(20).fontWeight('700').fontColor('#2B716A')
      Text(label).fontSize(10).fontColor('#81928D').margin({ top: 3 })
    }
    .layoutWeight(1).height(64).justifyContent(FlexAlign.Center)
    .backgroundColor('#F1F8F6').borderRadius(12)
  }

  @Builder
  PrivacyFlowRow(title: string, detail: string, badge: string, badgeColor: string): void {
    Row() {
      Column() {
        Text(title).fontSize(13).fontWeight('600').fontColor('#334A45')
        Text(detail).fontSize(10).fontColor('#879792').margin({ top: 3 }).maxLines(2)
      }
      .alignItems(HorizontalAlign.Start).layoutWeight(1)
      Text(badge).fontSize(9).fontColor(badgeColor)
        .backgroundColor('#F1F7F5').borderRadius(8)
        .padding({ left: 7, right: 7, top: 4, bottom: 4 })
    }
    .width('100%').padding({ top: 8, bottom: 8 })
  }

  @Builder
  PrivacyAuditRow(item: PrivacyAuditItem): void {
    Row() {
      Text(item.status === 'pass' ? '✓' : (item.status === 'warning' ? '!' : '×'))
        .fontSize(11).fontWeight('700').textAlign(TextAlign.Center)
        .fontColor(item.status === 'pass' ? '#397D77' : (item.status === 'warning' ? '#A36A2E' : '#C45F57'))
        .width(24).height(24).borderRadius(12)
        .backgroundColor(item.status === 'pass' ? '#EAF7F4' :
          (item.status === 'warning' ? '#FFF5E8' : '#FFF0EE'))
      Column() {
        Text(item.label).fontSize(12).fontWeight('600').fontColor('#334A45')
        Text(item.detail).fontSize(10).fontColor('#879792').margin({ top: 2 }).maxLines(3)
      }
      .alignItems(HorizontalAlign.Start).layoutWeight(1).margin({ left: 9 })
    }
    .width('100%').padding({ top: 7, bottom: 7 }).alignItems(VerticalAlign.Center)
  }

  @Builder
  PrivacyCenterPanel(): void {
    Column() {
      Row() {
        Column() {
          Text('隐私与记忆').fontSize(19).fontWeight('700').fontColor('#20332F')
          Text('你的数据由你查看、纠正和删除').fontSize(11).fontColor('#81928D').margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)
        Blank()
        Text('✕').fontSize(18).fontColor('#7F918C')
          .width(32).height(32).textAlign(TextAlign.Center)
          .backgroundColor('#F1F7F5').borderRadius(16)
          .onClick((): void => { if (!this.privacyBusy) this.isPrivacyPanelVisible = false; })
      }
      .width('100%').alignItems(VerticalAlign.Center).margin({ bottom: 12 })

      Scroll() {
        Column() {
          Row({ space: 8 }) {
            this.PrivacyCountCard('记忆', this.privacyFacts.length + this.privacyPreferences.length +
              this.privacyTopics.length)
            this.PrivacyCountCard('会话', this.historyList.length)
            this.PrivacyCountCard('备忘', this.memoItems.length)
            this.PrivacyCountCard('提醒', this.privacyReminderCount)
          }
          .width('100%')

          Text('助手记住的信息').fontSize(15).fontWeight('650').fontColor('#2C423D')
            .width('100%').margin({ top: 18, bottom: 8 })

          if (this.privacyFacts.length === 0 && this.privacyPreferences.length === 0 &&
              this.privacyTopics.length === 0) {
            Text('当前没有可展示的长期记忆')
              .fontSize(12).fontColor('#91A19C').width('100%')
              .padding(14).backgroundColor('#F8FBFA').borderRadius(12)
          }

          ForEach(this.privacyPreferences, (item: MemoryPreferenceItem) => {
            Row() {
              Column() {
                Text(item.key).fontSize(10).fontColor('#6F9189')
                Text(item.value).fontSize(13).fontColor('#334A45').margin({ top: 3 })
              }.alignItems(HorizontalAlign.Start).layoutWeight(1)
              Text('删除').fontSize(11).fontColor('#C4665E')
                .onClick((): void => this.confirmDeleteMemory('preference', item.key, item.key + '：' + item.value))
            }
            .width('100%').padding(12).margin({ bottom: 7 })
            .backgroundColor('#F8FBFA').borderRadius(12)
          }, (item: MemoryPreferenceItem): string => 'preference_' + item.key)

          ForEach(this.privacyFacts, (fact: string) => {
            Row() {
              Text(fact).fontSize(13).fontColor('#334A45').layoutWeight(1).maxLines(3)
              Text('删除').fontSize(11).fontColor('#C4665E').margin({ left: 10 })
                .onClick((): void => this.confirmDeleteMemory('fact', fact, fact))
            }
            .width('100%').padding(12).margin({ bottom: 7 })
            .backgroundColor('#F8FBFA').borderRadius(12)
          }, (fact: string): string => 'fact_' + fact)

          if (this.privacyTopics.length > 0) {
            Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
              ForEach(this.privacyTopics, (topic: string) => {
                Row({ space: 5 }) {
                  Text(topic).fontSize(11).fontColor('#397D77')
                  Text('×').fontSize(12).fontColor('#A36A62')
                    .onClick((): void => this.confirmDeleteMemory('topic', topic, '常聊话题：' + topic))
                }
                .padding({ left: 9, right: 8, top: 6, bottom: 6 })
                .margin({ right: 6, bottom: 6 })
                .backgroundColor('#EAF7F4').borderRadius(12)
              }, (topic: string): string => 'topic_' + topic)
            }
            .width('100%')
          }

          Text('累计完成 ' + this.privacyConversationCount + ' 次对话')
            .fontSize(10).fontColor('#91A19C').width('100%').margin({ top: 3 })

          Button('清空长期记忆')
            .width('100%').height(38).margin({ top: 10 })
            .fontSize(12).fontColor('#A85D56').backgroundColor('#FFF3F1').borderRadius(12)
            .enabled(!this.privacyBusy)
            .onClick((): void => this.confirmClearMemory())

          Divider().height(1).color('#E5EFEC').margin({ top: 18, bottom: 12 })
          Text('数据去向').fontSize(15).fontWeight('650').fontColor('#2C423D').width('100%')
          this.PrivacyFlowRow('记忆、会话与备忘', '保存在应用沙箱，并使用 HUKS 认证加密', '本地加密', '#397D77')
          this.PrivacyFlowRow('提醒通知', '提醒配置本地加密；触发内容需要交给系统通知服务', '系统托管', '#A36A2E')
          this.PrivacyFlowRow('模型请求', this.llmRoutingMode === ROUTING_LOCAL ?
            '当前纯本地模式，请求不会发送给远端模型' : '智能/远端模式下，部分问题可能发送到已配置服务',
            this.llmRoutingMode === ROUTING_LOCAL ? '仅本地' : '可能联网',
            this.llmRoutingMode === ROUTING_LOCAL ? '#397D77' : '#B26B43')

          Divider().height(1).color('#E5EFEC').margin({ top: 10, bottom: 12 })
          Row() {
            Column() {
              Text('本地存储安全检查').fontSize(15).fontWeight('650').fontColor('#2C423D')
              Text('仅检查文件头和元数据，不解密或展示正文')
                .fontSize(10).fontColor('#879792').margin({ top: 3 })
            }.alignItems(HorizontalAlign.Start).layoutWeight(1)
            Button(this.privacyAuditRunning ? '检查中…' : '立即检查')
              .height(32).fontSize(11).fontColor('#397D77')
              .backgroundColor('#EAF7F4').borderRadius(11)
              .enabled(!this.privacyAuditRunning && !this.privacyBusy)
              .onClick((): void => this.runPrivacyAudit())
          }
          .width('100%').alignItems(VerticalAlign.Center)

          if (this.privacyAuditSummary) {
            Text(this.privacyAuditSummary).fontSize(11)
              .fontColor(this.privacyAuditSummary.indexOf('失败') >= 0 ? '#C45F57' :
                (this.privacyAuditSummary.indexOf('警告') >= 0 ? '#A36A2E' : '#397D77'))
              .width('100%').padding(10).margin({ top: 9 })
              .backgroundColor('#F8FBFA').borderRadius(10)
          }

          ForEach(this.privacyAuditItems, (item: PrivacyAuditItem) => {
            this.PrivacyAuditRow(item)
          }, (item: PrivacyAuditItem): string => item.id)

          if (this.privacyStatus) {
            Text(this.privacyStatus).fontSize(11)
              .fontColor(this.privacyStatus.indexOf('失败') >= 0 || this.privacyStatus.indexOf('未全部') >= 0 ?
                '#C45F57' : '#397D77')
              .width('100%').padding(10).margin({ top: 10 })
              .backgroundColor('#F3F8F6').borderRadius(10)
          }

          Button(this.privacyBusy ? '正在处理…' : '清除全部个人内容')
            .width('100%').height(42).margin({ top: 14, bottom: 8 })
            .fontSize(13).fontColor('#FFFFFF')
            .backgroundColor(this.privacyBusy ? '#C9D6D2' : '#C45F57').borderRadius(13)
            .enabled(!this.privacyBusy)
            .onClick((): void => this.confirmClearPersonalData())
          Text('不会删除 ASR/TTS/LLM 模型文件和远端连接配置')
            .fontSize(9).fontColor('#98A7A3').width('100%').textAlign(TextAlign.Center)
        }
        .width('100%').alignItems(HorizontalAlign.Start)
      }
      .width('100%').layoutWeight(1)
    }
    .width('92%').height('82%')
    .backgroundColor('#FFFFFF').borderRadius(22).padding(18)
    .shadow({ radius: 22, offsetX: 0, offsetY: 8, color: 'rgba(31,58,51,0.22)' })
  }

  // ===== 子组件：欢迎区域 =====
  @Builder
  WelcomeSection(): void {
    Column() {
      Blank().height(34)
      Stack({ alignContent: Alignment.Center }) {
        Circle().width(84).height(84).fill('#E7F6F2')
        Circle().width(62).height(62).fill('#D2F0EA')
          .shadow({ radius: 22, offsetX: 0, offsetY: 8, color: 'rgba(69,175,168,0.22)' })
        Circle().width(38).height(38).fill('#72CBC3')
        Text('∿').fontSize(28).fontColor('#FFFFFF').fontWeight('600')
      }
      .width(96).height(96)

      Text('你好，我在这里')
        .fontSize(22).fontWeight('600').fontColor('#213A35').margin({ top: 16 })
      Text('可以直接说话，也可以输入文字')
        .fontSize(13).fontColor('#82938E').margin({ top: 7 })
      Row({ space: 6 }) {
        Text('⌾').fontSize(11).fontColor(PRIMARY_DARK)
        Text('本机处理 · 对话内容默认不上传').fontSize(11).fontColor('#5F817A')
      }
      .padding({ left: 12, right: 12, top: 6, bottom: 6 })
      .backgroundColor('#EAF7F4')
      .borderRadius(14)
      .margin({ top: 13 })

      Row({ space: 9 }) {
        this.SuggestionCard('✦', '写一份周报')
        this.SuggestionCard('⌘', '代码优化建议')
        this.SuggestionCard('↗', '学习路线规划')
      }
      .margin({ top: 28 })
    }
    .width('100%')
    .alignItems(HorizontalAlign.Center)
  }

  @Builder
  SuggestionCard(icon: string, label: string): void {
    Column() {
      Text(icon).fontSize(20).fontColor(PRIMARY_COLOR)
      Text(label).fontSize(11).fontColor('#536A64').margin({ top: 7 }).fontWeight('500').maxLines(1)
    }
    .width(98)
    .height(72)
    .backgroundColor('#FFFFFF')
    .borderRadius(16)
    .border({ width: 1, color: '#E7EFEC' })
    .shadow({ radius: 9, offsetX: 0, offsetY: 3, color: 'rgba(38,83,74,0.07)' })
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

  // ===== 子组件：自然呼吸语音交互坞 =====
  @Builder
  VoiceWaveform(): void {
    Row({ space: 4 }) {
      ForEach([8, 13, 19, 12, 24, 16, 28, 18, 23, 13, 20, 11, 7],
        (barHeight: number, index: number) => {
          Column()
            .width(3)
            .height(this.breathExpanded ? barHeight : Math.max(6, barHeight - 5))
            .backgroundColor(index === 6 ? this.voiceStateColor() : '#83CDC5')
            .borderRadius(2)
            .opacity(this.fullDuplexState === 'LLM_WAITING' ? 0.45 : 0.92)
            .animation({ duration: 1450, curve: Curve.EaseInOut })
        }, (barHeight: number, index: number): string => index.toString())
    }
    .height(32)
    .alignItems(VerticalAlign.Center)
    .justifyContent(FlexAlign.Center)
  }

  @Builder
  BreathingOrb(): void {
    Stack({ alignContent: Alignment.Center }) {
      Circle()
        .width(this.breathExpanded ? 84 : 74)
        .height(this.breathExpanded ? 84 : 74)
        .fill('rgba(111,203,194,0.10)')
        .animation({ duration: 1450, curve: Curve.EaseInOut })
      Circle()
        .width(this.breathExpanded ? 65 : 60)
        .height(this.breathExpanded ? 65 : 60)
        .fill('rgba(111,203,194,0.20)')
        .animation({ duration: 1450, curve: Curve.EaseInOut })
      Circle()
        .width(48).height(48)
        .fill(this.voiceStateColor())
        .shadow({ radius: 18, offsetX: 0, offsetY: 7, color: 'rgba(69,175,168,0.28)' })
      Text(this.fullDuplexState === 'TTS_PLAYING' ? '◖' : '∿')
        .fontSize(23).fontColor('#FFFFFF').fontWeight('600')
    }
    .width(90).height(88)
  }

  @Builder
  TextComposer(): void {
    Row({ space: 8 }) {
      TextArea({ placeholder: '输入消息…', text: this.inputText })
        .height(44)
        .layoutWeight(1)
        .backgroundColor('#F3F7F5')
        .borderRadius(22)
        .padding({ left: 15, right: 15 })
        .fontSize(14)
        .fontColor('#2F443F')
        .onChange((val: string): void => { this.inputText = val; })

      Button() {
        Text('↑').fontSize(19).fontColor('#FFFFFF').fontWeight('600')
      }
      .width(42).height(42)
      .backgroundColor(this.inputText.trim() ? PRIMARY_COLOR : '#C9D6D2')
      .borderRadius(21)
      .enabled(this.inputText.trim().length > 0)
      .onClick((): void => this.sendTextMsg())
    }
    .width('100%')
  }

  @Builder
  VoiceDock(): void {
    Column() {
      Row() {
        Row({ space: 7 }) {
          Circle().width(7).height(7).fill(this.voiceStateColor())
          Text(this.voiceStateLabel())
            .fontSize(13).fontColor(this.voiceStateColor()).fontWeight('600')
        }
        .padding({ left: 12, right: 12, top: 7, bottom: 7 })
        .backgroundColor('#EAF7F4')
        .borderRadius(16)
        Blank()
        Text(this.voiceStateHint()).fontSize(11).fontColor('#82948F')
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)

      Row() {
        this.BreathingOrb()
        Column() {
          this.VoiceWaveform()
          Text(this.voiceTranscript())
            .fontSize(this.asrInterimText ? 15 : 13)
            .fontColor(this.asrInterimText ? '#25443D' : '#82948F')
            .fontWeight(this.asrInterimText ? 500 : 400)
            .maxLines(2)
            .lineHeight(21)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
            .width('100%')
            .margin({ top: 4 })
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)
        .margin({ left: 4 })
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)
      .margin({ top: 8 })

      Row() {
        Button() {
          Text(this.keyboardInputVisible ? '⌄' : '⌨').fontSize(17).fontColor('#54736C')
        }
        .width(38).height(38).borderRadius(19).backgroundColor('#EFF6F4')
        .onClick((): void => { this.keyboardInputVisible = !this.keyboardInputVisible; })

        Blank()

        Button() {
          Text(this.fullDuplexActive ? '×' : (this.isRecording ? '■' : '●'))
            .fontSize(this.fullDuplexActive ? 24 : 15).fontColor('#FFFFFF').fontWeight('500')
        }
        .width(46).height(46).borderRadius(23)
        .backgroundColor(this.fullDuplexActive || this.isRecording ? '#4C6C65' : PRIMARY_COLOR)
        .shadow({ radius: 10, offsetX: 0, offsetY: 4, color: 'rgba(50,91,82,0.20)' })
        .onClick((): void => {
          if (this.fullDuplexActive) this.toggleFullDuplex();
          else this.toggleRecord();
        })

        Blank()

        Button() {
          Text('连续').fontSize(11).fontColor(this.fullDuplexActive ? PRIMARY_DARK : '#78908A')
        }
        .width(48).height(36).borderRadius(18)
        .backgroundColor(this.fullDuplexActive ? '#DFF3EF' : '#EFF6F4')
        .onClick((): void => this.toggleFullDuplex())
      }
      .width('100%')
      .alignItems(VerticalAlign.Center)
      .margin({ top: 6 })

      if (this.keyboardInputVisible) {
        Divider().height(1).color('#E1ECE8').margin({ top: 12, bottom: 10 })
        this.TextComposer()
      }
    }
    .width('100%')
    .padding({ left: 16, right: 16, top: 13, bottom: 12 })
    .background('linear-gradient(180deg, #FFFFFF, #F3FAF8)')
    .borderRadius(24)
    .border({ width: 1, color: '#DDECE8' })
    .shadow({ radius: 20, offsetX: 0, offsetY: -2, color: 'rgba(37,86,76,0.12)' })
  }

  @Builder
  BottomInput(): void {
    Column() {
      if (this.shouldShowNotice()) {
        Row({ space: 7 }) {
          Circle().width(6).height(6).fill('#D29A52')
          Text(this.logInfo)
            .fontSize(11).fontColor('#7A6A52').maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 7, bottom: 7 })
        .backgroundColor('#FFF9EE')
        .borderRadius(12)
        .margin({ bottom: 7 })
      }

      if (this.shouldShowVoiceDock()) {
        this.VoiceDock()
      } else {
        Row({ space: 8 }) {
          Button() {
            Stack({ alignContent: Alignment.Center }) {
              Circle().width(40).height(40).fill(PRIMARY_COLOR)
                .shadow({ radius: 10, offsetX: 0, offsetY: 4, color: 'rgba(69,175,168,0.22)' })
              Text('∿').fontSize(21).fontColor('#FFFFFF').fontWeight('600')
            }
          }
          .width(44).height(44).backgroundColor('transparent')
          .onClick((): void => this.toggleRecord())

          Column() {
            this.TextComposer()
          }
          .layoutWeight(1)

          Button() {
            Text('连续').fontSize(10).fontColor('#5D7A73')
          }
          .width(42).height(36).borderRadius(18).backgroundColor('#EFF6F4')
          .onClick((): void => this.toggleFullDuplex())
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)
        .padding({ left: 10, right: 8, top: 8, bottom: 8 })
        .backgroundColor('#FFFFFF')
        .borderRadius(24)
        .border({ width: 1, color: '#E3ECE9' })
        .shadow({ radius: 14, offsetX: 0, offsetY: -1, color: 'rgba(36,74,66,0.09)' })
      }
    }
    .width('100%')
    .padding({ left: 12, right: 12, top: 8, bottom: 10 })
    .backgroundColor(BG_COLOR)
  }
}
