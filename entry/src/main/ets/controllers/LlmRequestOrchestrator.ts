import {
  decideLlmRoute,
  LlmRouteDecision,
  ROUTING_SMART
} from '../utils/LlmRoutePolicy';
import {
  RemoteChatMessage,
  RemoteLlmClient,
  RemoteStreamFailure
} from '../services/RemoteLlmClient';

const REMOTE_COOLDOWN_MS: number = 30000;

export interface LlmRequestEnvironment {
  mode: number;
  userText: string;
  localInstalled: boolean;
  online: boolean;
  remoteConfigured: boolean;
  remoteSourceLabel: string;
  remoteUrl: string;
  remoteApiKey: string;
  remoteModelName: string;
}

export interface LlmRequestCallbacks {
  onToken: (token: string) => void;
  onComplete: (fullText: string) => void;
  onRoute?: (source: string) => void;
}

export interface RemoteStateUpdate {
  verified: boolean;
  configStatus: string;
  localStatus: string;
}

export interface LlmRequestDependencies {
  runLocal: (
    userText: string,
    onToken: (token: string) => void,
    onComplete: (fullText: string) => void
  ) => void;
  buildRemoteMessages: (userText: string) => RemoteChatMessage[];
  appendContext: (userText: string, assistantText: string) => void;
  scheduleLocalRelease: () => void;
  captureMemory: (label: string) => void;
  updateRemoteState: (update: RemoteStateUpdate) => void;
  updateLogInfo: (message: string) => void;
}

/**
 * Owns one LLM request's routing and fallback lifecycle. UI state and local
 * inference remain injected dependencies, so this class has no page lifecycle.
 */
export class LlmRequestOrchestrator {
  private remoteClient: RemoteLlmClient;
  private remoteUnavailableUntil: number = 0;

  constructor(remoteClient: RemoteLlmClient = new RemoteLlmClient()) {
    this.remoteClient = remoteClient;
  }

  execute(
    environment: LlmRequestEnvironment,
    dependencies: LlmRequestDependencies,
    callbacks: LlmRequestCallbacks
  ): void {
    dependencies.captureMemory('LLM请求前');
    const route: LlmRouteDecision = decideLlmRoute({
      mode: environment.mode,
      userText: environment.userText,
      localInstalled: environment.localInstalled,
      online: environment.online,
      remoteConfigured: environment.remoteConfigured,
      remoteCoolingDown: this.isRemoteCoolingDown(),
      remoteSourceLabel: environment.remoteSourceLabel
    });
    console.info('OHOS_ROUTE: mode=' + environment.mode + ' target=' + route.target +
      ' reason=' + route.reason);

    if (route.target === 'refuse') {
      callbacks.onRoute?.(route.reason);
      this.completeWithMessage(environment.userText, route.refusal, dependencies, callbacks, true);
      return;
    }
    if (route.target === 'local') {
      callbacks.onRoute?.(route.reason);
      dependencies.runLocal(environment.userText, callbacks.onToken, callbacks.onComplete);
      return;
    }
    this.runRemote(environment, route, dependencies, callbacks);
  }

  private isRemoteCoolingDown(): boolean {
    return Date.now() < this.remoteUnavailableUntil;
  }

  private runRemote(
    environment: LlmRequestEnvironment,
    route: LlmRouteDecision,
    dependencies: LlmRequestDependencies,
    callbacks: LlmRequestCallbacks
  ): void {
    if (!environment.remoteConfigured) {
      const message: string = '请先配置云端 API / 自建 Ollama，或下载本地 GGUF 模型。';
      callbacks.onRoute?.('⚙️ 远端未配置');
      dependencies.updateLogInfo(message);
      callbacks.onComplete(message);
      return;
    }
    if (!environment.online && environment.localInstalled) {
      if (environment.mode === ROUTING_SMART) {
        const message: string = '事实查询需要远端模型，但网络已断开。为避免离线小模型编造，本轮不自动降级。';
        callbacks.onRoute?.('⚠️ 未发送 · 智能路由保护');
        this.completeWithMessage(environment.userText, message, dependencies, callbacks, true);
        return;
      }
      console.info('OHOS_LLM: offline, auto-fallback to local');
      callbacks.onRoute?.('📱 本机 GGUF（离线）');
      dependencies.runLocal(environment.userText, callbacks.onToken, callbacks.onComplete);
      return;
    }
    if (!environment.online) {
      const message: string = '当前无网络且本机 GGUF 尚未下载，请联网下载模型后再试。';
      callbacks.onRoute?.('⚙️ 无可用模型');
      dependencies.updateLogInfo(message);
      callbacks.onComplete(message);
      return;
    }
    if (this.isRemoteCoolingDown() && environment.localInstalled) {
      if (environment.mode === ROUTING_SMART) {
        const message: string = '远端模型暂时不可用。为避免用离线小模型回答事实问题，本轮不自动降级。';
        callbacks.onRoute?.('⚠️ 未发送 · 远端冷却');
        this.completeWithMessage(environment.userText, message, dependencies, callbacks, true);
        return;
      }
      console.info('OHOS_LLM: remote cooldown active, using local for this request');
      callbacks.onRoute?.('📱 本机 GGUF（远端冷却）');
      dependencies.runLocal(environment.userText, callbacks.onToken, callbacks.onComplete);
      return;
    }

    dependencies.scheduleLocalRelease();
    callbacks.onRoute?.(route.reason);
    this.remoteClient.stream({
      url: environment.remoteUrl,
      apiKey: environment.remoteApiKey,
      modelName: environment.remoteModelName,
      messages: dependencies.buildRemoteMessages(environment.userText),
      maxTokens: 512,
      temperature: 0.7
    }, {
      onToken: callbacks.onToken,
      onVerified: (): void => {
        this.remoteUnavailableUntil = 0;
        dependencies.updateRemoteState({
          verified: true,
          configStatus: '已验证可用',
          localStatus: ''
        });
      },
      onComplete: (fullText: string): void => {
        dependencies.captureMemory('远端LLM完成');
        dependencies.appendContext(environment.userText, fullText);
        callbacks.onComplete(fullText);
      },
      onFailure: (failure: RemoteStreamFailure): void => {
        this.handleRemoteFailure(environment, failure, dependencies, callbacks);
      }
    });
  }

  private handleRemoteFailure(
    environment: LlmRequestEnvironment,
    failure: RemoteStreamFailure,
    dependencies: LlmRequestDependencies,
    callbacks: LlmRequestCallbacks
  ): void {
    if (environment.localInstalled && environment.mode !== ROUTING_SMART) {
      this.remoteUnavailableUntil = Date.now() + REMOTE_COOLDOWN_MS;
      dependencies.updateRemoteState({
        verified: false,
        configStatus: '连接失败，已切换本地模型',
        localStatus: '远端不可用，正在切换本地模型'
      });
      callbacks.onRoute?.('📱 远端失败 → 本机 GGUF');
      console.warn('OHOS_LLM: remote failed before first token, fallback local reason=' + failure.reason);
      dependencies.runLocal(environment.userText, callbacks.onToken, callbacks.onComplete);
      return;
    }

    dependencies.updateRemoteState({
      verified: false,
      configStatus: failure.kind === 'http' ?
        '连接失败，请检查服务地址或 API Key' : '连接异常，请检查网络和服务地址',
      localStatus: ''
    });
    if (environment.mode === ROUTING_SMART) {
      const result = this.buildSmartFailureResult(failure);
      callbacks.onRoute?.(result.route);
      this.completeWithMessage(environment.userText, result.message, dependencies, callbacks, true);
    } else if (failure.kind === 'http') {
      this.completeWithMessage(environment.userText,
        '调用大模型失败，状态码：' + failure.statusCode, dependencies, callbacks, false);
    } else if (failure.kind !== 'empty') {
      this.completeWithMessage(environment.userText,
        '调用大模型失败，请检查网络和 API 配置。', dependencies, callbacks, false);
    } else {
      callbacks.onComplete('');
    }
  }

  private buildSmartFailureResult(failure: RemoteStreamFailure): Record<string, string> {
    if (failure.kind === 'empty') {
      return {
        message: '远端没有返回有效内容，本轮未降级到离线小模型，以避免生成未经核实的答案。',
        route: '⚠️ 远端空响应 · 未降级'
      };
    }
    if (failure.kind === 'http') {
      return {
        message: '远端事实查询失败（状态码 ' + failure.statusCode + '），本轮未降级到离线小模型。',
        route: '⚠️ 远端失败 · 未降级'
      };
    }
    return {
      message: '远端事实查询连接异常，本轮未降级到离线小模型。',
      route: '⚠️ 远端异常 · 未降级'
    };
  }

  private completeWithMessage(
    userText: string,
    message: string,
    dependencies: LlmRequestDependencies,
    callbacks: LlmRequestCallbacks,
    appendContext: boolean
  ): void {
    if (appendContext) dependencies.appendContext(userText, message);
    callbacks.onToken(message);
    callbacks.onComplete(message);
  }
}
