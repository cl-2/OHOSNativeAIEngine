import worker, { MessageEvents } from '@ohos.worker';

export interface TtsWorkerEvent {
  type: string;
  reason: string;
}

export interface AsrWorkerEvent {
  type: string;
  text: string;
  isFinal: boolean;
  error: string;
}

export interface AsrPrewarmEvent {
  success: boolean;
  durationMs: number;
  error: string;
}

/** Keeps Worker creation/reuse and message protocol out of the ArkUI page. */
export class SpeechWorkerCoordinator {
  private ttsWorker?: worker.ThreadWorker;
  private asrWorker?: worker.ThreadWorker;
  private prewarmedAsrWorker?: worker.ThreadWorker;
  private ttsEventCallback?: (event: TtsWorkerEvent) => void;

  get hasAsrWorker(): boolean { return this.asrWorker !== undefined; }
  get hasTtsWorker(): boolean { return this.ttsWorker !== undefined; }

  initializeTts(onEvent: (event: TtsWorkerEvent) => void): void {
    this.ttsEventCallback = onEvent;
    this.ensureTtsWorker();
  }

  private ensureTtsWorker(): boolean {
    if (this.ttsWorker) return true;
    try {
      const instance: worker.ThreadWorker = new worker.ThreadWorker(
        'entry/ets/workers/TtsWorker.ets', { name: 'TTS Worker' });
      instance.onmessage = (message: MessageEvents): void => {
        this.ttsEventCallback?.({
          type: message.data['msgType'] as string,
          reason: (message.data['reason'] as string) || ''
        });
      };
      this.ttsWorker = instance;
      return true;
    } catch (error) {
      console.error('OHOS_FLOW: failed to create TTS worker: ' + String(error));
      return false;
    }
  }

  startTts(text: string, modelDir: string, speed: number): boolean {
    if (!this.ensureTtsWorker()) return false;
    try {
      this.ttsWorker?.postMessage({ msgType: 'startTts', text, modelDir, speed });
      return true;
    } catch (error) {
      console.error('OHOS_FLOW: startTts worker post failed=' + String(error));
      this.ttsWorker = undefined;
      return false;
    }
  }

  sendTtsCommand(command: string): void {
    this.ttsWorker?.postMessage({ msgType: command });
  }

  prewarmAsr(modelDir: string, vadModelPath: string,
    onComplete: (event: AsrPrewarmEvent) => void): void {
    const instance: worker.ThreadWorker = new worker.ThreadWorker(
      'entry/ets/workers/AsrWorker.ets', { name: 'ASR Prewarm' });
    instance.onmessage = (message: MessageEvents): void => {
      if ((message.data['msgType'] as string) !== 'asr-prewarm-done') return;
      onComplete({
        success: message.data['success'] as boolean,
        durationMs: message.data['durationMs'] as number,
        error: (message.data['error'] as string) || ''
      });
    };
    instance.postMessage({ msgType: 'prewarmAsr', asrModelDir: modelDir, vadModelPath });
    this.prewarmedAsrWorker = instance;
  }

  startAsr(modelDir: string, vadModelPath: string,
    onEvent: (event: AsrWorkerEvent) => void): void {
    if (!this.asrWorker) {
      this.asrWorker = this.prewarmedAsrWorker || new worker.ThreadWorker(
        'entry/ets/workers/AsrWorker.ets', { name: 'ASR Worker' });
      this.prewarmedAsrWorker = undefined;
      this.asrWorker.onmessage = (message: MessageEvents): void => {
        onEvent({
          type: message.data['msgType'] as string,
          text: (message.data['text'] as string) || '',
          isFinal: message.data['isFinal'] as boolean,
          error: (message.data['error'] as string) || ''
        });
      };
    }
    this.asrWorker.postMessage({ msgType: 'startAsr', asrModelDir: modelDir, vadModelPath });
  }

  stopAsr(): void {
    this.asrWorker?.postMessage({ msgType: 'stopAsr' });
  }

  releaseTtsRenderer(): void {
    this.sendTtsCommand('releaseTtsRenderer');
  }
}
