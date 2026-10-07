/** OpenAI-compatible SSE transport. Routing and fallback policy stay outside. */
import { http } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';

export interface RemoteChatMessage {
  role: string;
  content: string;
}

export interface RemoteStreamRequest {
  url: string;
  apiKey: string;
  modelName: string;
  messages: RemoteChatMessage[];
  maxTokens: number;
  temperature: number;
}

export type RemoteFailureKind = 'empty' | 'http' | 'network' | 'startup';

export interface RemoteStreamFailure {
  kind: RemoteFailureKind;
  reason: string;
  statusCode: number;
}

export interface RemoteStreamCallbacks {
  onToken: (token: string) => void;
  onComplete: (fullText: string) => void;
  onFailure: (failure: RemoteStreamFailure) => void;
  onVerified?: () => void;
}

export interface RemoteRequestHandle {
  cancel(): void;
}

/** Preserves an incomplete SSE line across arbitrary network chunk boundaries. */
export class SseContentParser {
  private carry: string = '';

  push(chunk: string): string[] {
    const text: string = this.carry + chunk;
    const lines: string[] = text.split('\n');
    this.carry = lines.pop() || '';
    return this.parseLines(lines);
  }

  finish(): string[] {
    if (!this.carry) return [];
    const lines: string[] = [this.carry];
    this.carry = '';
    return this.parseLines(lines);
  }

  private parseLines(lines: string[]): string[] {
    const contents: string[] = [];
    for (let i: number = 0; i < lines.length; i++) {
      const line: string = lines[i].trimEnd();
      if (!line.startsWith('data: ')) continue;
      const data: string = line.substring(6).trim();
      if (!data || data === '[DONE]') continue;
      try {
        const json: Record<string, Object> = JSON.parse(data) as Record<string, Object>;
        const choices: Object[] = json['choices'] as Object[];
        if (!choices || choices.length === 0) continue;
        const choice: Record<string, Object> = choices[0] as Record<string, Object>;
        const delta: Record<string, string> = choice['delta'] as Record<string, string>;
        const content: string = delta ? delta['content'] || '' : '';
        if (content) contents.push(content);
      } catch (_) {}
    }
    return contents;
  }
}

export class RemoteLlmClient {
  stream(request: RemoteStreamRequest, callbacks: RemoteStreamCallbacks): RemoteRequestHandle {
    const httpReq: http.HttpRequest = http.createHttp();
    const decoder: util.TextDecoder = util.TextDecoder.create('utf-8', { fatal: false });
    const parser: SseContentParser = new SseContentParser();
    let fullText: string = '';
    let pendingBatch: string = '';
    let settled: boolean = false;
    let verified: boolean = false;

    const uiTimer: number = setInterval((): void => {
      if (!pendingBatch) return;
      const batch: string = pendingBatch;
      pendingBatch = '';
      callbacks.onToken(batch);
    }, 30);

    const destroy = (): void => {
      try { httpReq.destroy(); } catch (_) {}
    };
    const flush = (): void => {
      if (pendingBatch) {
        const batch: string = pendingBatch;
        pendingBatch = '';
        callbacks.onToken(batch);
      }
    };
    const complete = (): void => {
      if (settled) return;
      settled = true;
      clearInterval(uiTimer);
      flush();
      destroy();
      if (fullText) callbacks.onComplete(fullText);
      else callbacks.onFailure({ kind: 'empty', reason: 'empty response', statusCode: 0 });
    };
    const fail = (kind: RemoteFailureKind, reason: string, statusCode: number = 0): void => {
      if (settled) return;
      settled = true;
      clearInterval(uiTimer);
      destroy();
      if (fullText) {
        flush();
        callbacks.onComplete(fullText);
      } else {
        callbacks.onFailure({ kind, reason, statusCode });
      }
    };
    const acceptContents = (contents: string[]): void => {
      for (let i: number = 0; i < contents.length; i++) {
        fullText += contents[i];
        pendingBatch += contents[i];
        if (!verified) {
          verified = true;
          callbacks.onVerified?.();
        }
      }
    };

    httpReq.on('dataReceive', (data: ArrayBuffer): void => {
      if (settled) return;
      const chunk: string = decoder.decodeWithStream(new Uint8Array(data));
      acceptContents(parser.push(chunk));
    });
    httpReq.on('dataEnd', (): void => {
      if (settled) return;
      acceptContents(parser.finish());
      complete();
    });

    const body: string = JSON.stringify({
      model: request.modelName,
      messages: request.messages,
      max_tokens: request.maxTokens,
      temperature: request.temperature,
      stream: true
    });
    try {
      httpReq.requestInStream(request.url, {
        method: http.RequestMethod.POST,
        header: {
          'Content-Type': 'application/json',
          'Authorization': request.apiKey ? 'Bearer ' + request.apiKey : ''
        },
        extraData: body,
        connectTimeout: 15000,
        readTimeout: 60000
      }).then((code: number): void => {
        if (code !== 200) fail('http', 'http ' + code, code);
      }).catch((error: Error): void => {
        console.error('OHOS_LLM_STREAM_FAILED: ' + JSON.stringify(error));
        fail('network', 'request exception');
      });
    } catch (error) {
      console.error('OHOS_LLM_STREAM_START_FAILED: ' + JSON.stringify(error));
      fail('startup', 'request startup exception');
    }

    return {
      cancel: (): void => fail('network', 'cancelled')
    };
  }
}
