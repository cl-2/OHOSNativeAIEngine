/**
 * LlmWorker - 后台加载 LLM 引擎（不阻塞 UI 线程）
 */
import worker, { ThreadWorkerGlobalScope, MessageEvents } from '@ohos.worker';
import nativeLib from 'libnative_lib.so';

const workerPort: ThreadWorkerGlobalScope = worker.workerPort;

workerPort.onmessage = (e: MessageEvents): void => {
  const msgType: string = e.data['msgType'] as string;

  if (msgType === 'prewarmLlm') {
    const modelDir: string = e.data['modelDir'] as string;
    console.info('OHOS_LLM_WORKER: prewarming...');
    try {
      const ok: boolean = nativeLib.initLocalLlm(modelDir) as boolean;
      workerPort.postMessage({ msgType: 'llm-prewarm-result', success: ok });
      console.info('OHOS_LLM_WORKER: prewarm result=' + ok);
    } catch (e) {
      console.error('OHOS_LLM_WORKER: error=' + String(e));
      workerPort.postMessage({ msgType: 'llm-prewarm-result', success: false });
    }
  }
};
