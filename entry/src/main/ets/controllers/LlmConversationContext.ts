/** Pure multi-turn context and prompt-budget controller. */

export interface LlmContextMessage {
  role: string;
  content: string;
}

export interface ContextChatItem {
  msg: string;
  from: 'user' | 'assistant';
}

const REMOTE_PROMPT_TOKEN_BUDGET: number = 1400;
const LOCAL_PROMPT_TOKEN_BUDGET: number = 300;
const LOCAL_HISTORY_USER_MAX_CHARS: number = 64;
const LOCAL_HISTORY_ASSISTANT_MAX_CHARS: number = 112;
const MAX_HISTORY_MESSAGES: number = 20;

export class LlmConversationContext {
  private history: LlmContextMessage[] = [];

  buildPrelude(userText: string, memoryPrompt: string, ragContext: string,
    remoteMode: boolean = false): LlmContextMessage[] {
    let systemContent: string = remoteMode ?
      '你是远端智能助手，请用简洁友好的方式回答。不得编造人物经历、日期、作品或其他事实。' +
        '当前请求没有接入网页检索；如果无法从可靠知识中确认，请明确说明不确定，不要假装掌握实时信息。' :
      '你是一个端侧智能助手，请用简洁友好的方式回答。不得编造人物经历、日期、作品或其他事实；' +
        '如果现有上下文不足以确认，请明确说明“我无法在离线知识中确认”，并建议用户联网查询。';
    if (!remoteMode) {
      systemContent += '语音回答默认控制在2到4句话，先给结论再补充必要信息；' +
        '只有用户明确要求详细说明时才增加要点，避免冗长重复。';
    }
    if (memoryPrompt) systemContent += '\n\n关于用户的信息：\n' + memoryPrompt;
    if (ragContext) systemContent += '\n\n' + ragContext;
    systemContent += '\n\n请用这些信息来提供更个性化的回答。';

    const systemMessage: LlmContextMessage = { role: 'system', content: systemContent };
    const selectedHistory: LlmContextMessage[] = [];
    let usedTokens: number = this.estimateTokens(systemContent) + this.estimateTokens(userText) + 24;
    const maxHistoryPairs: number = remoteMode ? 10 : 2;
    const promptTokenBudget: number = remoteMode ? REMOTE_PROMPT_TOKEN_BUDGET : LOCAL_PROMPT_TOKEN_BUDGET;
    let selectedPairs: number = 0;
    let cursor: number = this.history.length - 1;

    while (cursor >= 1 && selectedPairs < maxHistoryPairs) {
      const assistantMessage: LlmContextMessage = this.history[cursor];
      const rawUserMessage: LlmContextMessage = this.history[cursor - 1];
      if (assistantMessage.role !== 'assistant' || rawUserMessage.role !== 'user') {
        cursor--;
        continue;
      }
      const cleanUserContent: string = this.stripLegacyInterruptedContext(rawUserMessage.content);
      const userMessage: LlmContextMessage = {
        role: 'user',
        content: remoteMode ? cleanUserContent :
          this.compactLocalHistoryText(cleanUserContent, LOCAL_HISTORY_USER_MAX_CHARS)
      };
      const boundedAssistantMessage: LlmContextMessage = {
        role: 'assistant',
        content: remoteMode ? assistantMessage.content :
          this.compactLocalHistoryText(assistantMessage.content, LOCAL_HISTORY_ASSISTANT_MAX_CHARS)
      };
      const pairTokens: number = this.estimateTokens(userMessage.content) +
        this.estimateTokens(boundedAssistantMessage.content) + 12;
      if (usedTokens + pairTokens > promptTokenBudget) break;
      selectedHistory.unshift(userMessage, boundedAssistantMessage);
      usedTokens += pairTokens;
      selectedPairs++;
      cursor -= 2;
    }
    const droppedMessages: number = this.history.length - selectedHistory.length;
    if (droppedMessages > 0) {
      console.info('OHOS_CTX: budget=' + promptTokenBudget + ' estimated=' + usedTokens +
        ' kept=' + selectedHistory.length + ' dropped=' + droppedMessages);
    }
    return [systemMessage, ...selectedHistory];
  }

  append(userText: string, assistantText: string): void {
    if (!userText || !assistantText) return;
    this.history.push({ role: 'user', content: userText });
    this.history.push({ role: 'assistant', content: assistantText });
    if (this.history.length > MAX_HISTORY_MESSAGES) {
      this.history = this.history.slice(-MAX_HISTORY_MESSAGES);
    }
  }

  rebuild(chatItems: ContextChatItem[]): void {
    const rebuilt: LlmContextMessage[] = [];
    for (let i: number = 0; i < chatItems.length; i++) {
      const item: ContextChatItem = chatItems[i];
      if (!item.msg) continue;
      if (item.from === 'user') {
        rebuilt.push({ role: 'user', content: this.stripLegacyInterruptedContext(item.msg) });
      } else if (rebuilt.length > 0 && rebuilt[rebuilt.length - 1].role === 'user') {
        rebuilt.push({ role: 'assistant', content: item.msg });
      }
    }
    this.history = rebuilt.length > MAX_HISTORY_MESSAGES ?
      rebuilt.slice(-MAX_HISTORY_MESSAGES) : rebuilt;
  }

  clear(): void {
    this.history = [];
  }

  getHistoryForTest(): LlmContextMessage[] {
    return this.history.slice();
  }

  private estimateTokens(text: string): number {
    let asciiChars: number = 0;
    let nonAsciiChars: number = 0;
    for (let i: number = 0; i < text.length; i++) {
      const code: number = text.charCodeAt(i);
      if (code <= 0x7F) asciiChars++;
      else {
        nonAsciiChars++;
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.length) i++;
      }
    }
    return Math.ceil(asciiChars / 3) + nonAsciiChars + 4;
  }

  private stripLegacyInterruptedContext(text: string): string {
    const marker: string = '\n\n[AI 之前说到:';
    const markerIndex: number = text.indexOf(marker);
    return markerIndex >= 0 ? text.substring(0, markerIndex).trim() : text;
  }

  private compactLocalHistoryText(text: string, maxChars: number): string {
    if (text.length <= maxChars) return text;
    const marker: string = '…';
    const contentChars: number = Math.max(16, maxChars - marker.length);
    const headChars: number = Math.floor(contentChars * 0.72);
    const tailChars: number = contentChars - headChars;
    return text.substring(0, headChars).trim() + marker +
      text.substring(text.length - tailChars).trim();
  }
}
