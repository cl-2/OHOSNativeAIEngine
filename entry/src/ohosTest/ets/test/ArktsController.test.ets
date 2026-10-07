import { describe, it, expect } from '@ohos/hypium';
import { LlmConversationContext, LlmContextMessage } from '../../../main/ets/controllers/LlmConversationContext';
import {
  isRemoteConfigComplete,
  normalizeRemoteUrl,
  StoredRemoteLlmConfig
} from '../../../main/ets/controllers/LlmSettingsStore';
import { LocalControlResult, LocalSkillController } from '../../../main/ets/controllers/LocalSkillController';
import { LlmRequestOrchestrator } from '../../../main/ets/controllers/LlmRequestOrchestrator';
import { ConversationSessionController } from '../../../main/ets/controllers/ConversationSessionController';
import { ROUTING_LOCAL, ROUTING_SMART } from '../../../main/ets/utils/LlmRoutePolicy';
import { SseContentParser } from '../../../main/ets/services/RemoteLlmClient';
import { isBundledModelFileValid } from '../../../main/ets/services/SpeechModelInstaller';

export default function arktsControllerTest(): void {
  describe('ArktsControllerTest', (): void => {
    it('normalizesBareOllamaAddress', 0, (): void => {
      expect(normalizeRemoteUrl('192.168.18.226', true))
        .assertEqual('http://192.168.18.226:11434/v1/chat/completions');
    });

    it('keepsCompleteCloudUrl', 0, (): void => {
      const url: string = 'https://api.example.com/v1/chat/completions';
      expect(normalizeRemoteUrl(url, false)).assertEqual(url);
    });

    it('requiresApiKeyOnlyForCloudProvider', 0, (): void => {
      const config: StoredRemoteLlmConfig = {
        providerIndex: 0,
        apiUrl: '192.168.18.226',
        modelName: 'qwen2.5:7b',
        apiKey: ''
      };
      expect(isRemoteConfigComplete(config, true)).assertTrue();
      expect(isRemoteConfigComplete(config, false)).assertFalse();
    });

    it('rebuildsConversationWithoutLegacyInterruptedSuffix', 0, (): void => {
      const context: LlmConversationContext = new LlmConversationContext();
      context.rebuild([
        { msg: '继续说明\n\n[AI 之前说到:旧内容]', from: 'user' },
        { msg: '新的回答', from: 'assistant' }
      ]);
      const history: LlmContextMessage[] = context.getHistoryForTest();
      expect(history.length).assertEqual(2);
      expect(history[0].content).assertEqual('继续说明');
    });

    it('boundsStoredConversationHistory', 0, (): void => {
      const context: LlmConversationContext = new LlmConversationContext();
      for (let i: number = 0; i < 12; i++) context.append('问题' + i, '回答' + i);
      expect(context.getHistoryForTest().length).assertEqual(20);
    });

    it('resolvesStopWithoutRestartingSpeech', 0, (): void => {
      const controller: LocalSkillController = new LocalSkillController();
      const result: LocalControlResult = controller.resolveControl({
        matched: true,
        skill: 'stop_speaking',
        normalizedText: '停止朗读'
      }, {
        previousAssistantReply: '',
        runtimeStatus: '',
        speed: 1.0,
        localModelInstalled: true,
        remoteConfigured: true
      });
      expect(result.effect).assertEqual('stop_tts');
      expect(result.speechText).assertEqual('');
    });

    it('clampsSpeechSpeed', 0, (): void => {
      const controller: LocalSkillController = new LocalSkillController();
      const result: LocalControlResult = controller.resolveControl({
        matched: true,
        skill: 'speed_up',
        normalizedText: '语速快一点'
      }, {
        previousAssistantReply: '',
        runtimeStatus: '',
        speed: 1.3,
        localModelInstalled: true,
        remoteConfigured: true
      });
      expect(result.nextSpeed).assertEqual(1.3);
    });

    it('parsesSseAcrossNetworkChunkBoundary', 0, (): void => {
      const parser: SseContentParser = new SseContentParser();
      expect(parser.push('data: {"choices":[{"delta":{"content":"你').length).assertEqual(0);
      const contents: string[] = parser.push('好"}}]}\n');
      expect(contents.join('')).assertEqual('你好');
    });

    it('validatesKnownAndVariableSizeModelFiles', 0, (): void => {
      expect(isBundledModelFileValid(100, 100)).assertTrue();
      expect(isBundledModelFileValid(99, 100)).assertFalse();
      expect(isBundledModelFileValid(1, 0)).assertTrue();
      expect(isBundledModelFileValid(0, 0)).assertFalse();
    });

    it('orchestratesExplicitLocalRequestWithoutRemoteTransport', 0, (): void => {
      const orchestrator: LlmRequestOrchestrator = new LlmRequestOrchestrator();
      let localCalled: boolean = false;
      let route: string = '';
      orchestrator.execute({
        mode: ROUTING_LOCAL,
        userText: '本地回答',
        localInstalled: true,
        online: true,
        remoteConfigured: true,
        remoteSourceLabel: '远端',
        remoteUrl: '',
        remoteApiKey: '',
        remoteModelName: ''
      }, {
        runLocal: (_text: string, _onToken: (token: string) => void,
          onComplete: (fullText: string) => void): void => {
          localCalled = true;
          onComplete('本地结果');
        },
        buildRemoteMessages: (_text: string): LlmContextMessage[] => [],
        appendContext: (_userText: string, _assistantText: string): void => {},
        scheduleLocalRelease: (): void => {},
        captureMemory: (_label: string): void => {},
        updateRemoteState: (): void => {},
        updateLogInfo: (_message: string): void => {}
      }, {
        onToken: (_token: string): void => {},
        onComplete: (_fullText: string): void => {},
        onRoute: (source: string): void => { route = source; }
      });
      expect(localCalled).assertTrue();
      expect(route.includes('本机 GGUF')).assertTrue();
    });

    it('keepsPrivacyRefusalLocalToOrchestrator', 0, (): void => {
      const orchestrator: LlmRequestOrchestrator = new LlmRequestOrchestrator();
      let appended: string = '';
      let completed: string = '';
      orchestrator.execute({
        mode: ROUTING_SMART,
        userText: '我的身份证是多少',
        localInstalled: false,
        online: true,
        remoteConfigured: true,
        remoteSourceLabel: '远端',
        remoteUrl: '',
        remoteApiKey: '',
        remoteModelName: ''
      }, {
        runLocal: (_text: string, _onToken: (token: string) => void,
          _onComplete: (fullText: string) => void): void => {},
        buildRemoteMessages: (_text: string): LlmContextMessage[] => [],
        appendContext: (_userText: string, assistantText: string): void => { appended = assistantText; },
        scheduleLocalRelease: (): void => {},
        captureMemory: (_label: string): void => {},
        updateRemoteState: (): void => {},
        updateLogInfo: (_message: string): void => {}
      }, {
        onToken: (_token: string): void => {},
        onComplete: (fullText: string): void => { completed = fullText; }
      });
      expect(completed.length > 0).assertTrue();
      expect(appended).assertEqual(completed);
    });

    it('invalidatesFirstTurnCallbackAfterStartingNewConversation', 0, (): void => {
      const controller: ConversationSessionController = new ConversationSessionController();
      const firstId: string = controller.ensureCurrentConversationId();
      expect(firstId.length > 0).assertTrue();
      expect(controller.isCurrent(firstId)).assertTrue();
      controller.startNewConversation();
      expect(controller.isCurrent(firstId)).assertFalse();
    });
  });
}
