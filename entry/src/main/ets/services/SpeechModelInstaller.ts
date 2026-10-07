/** Installs bundled ASR/VAD/TTS assets into the application sandbox. */
import { common } from '@kit.AbilityKit';
import { fileIo } from '@kit.CoreFileKit';
import nativeLib from 'libnative_lib.so';

interface BundledModelFile {
  rawPath: string;
  relativeDest: string;
  expectedSize: number;
  critical: boolean;
}

const CORE_MODEL_FILES: BundledModelFile[] = [
  { rawPath: 'models/asr/encoder-epoch-99-avg-1.int8.onnx', relativeDest: 'asr/encoder-epoch-99-avg-1.int8.onnx', expectedSize: 161141793, critical: true },
  { rawPath: 'models/asr/decoder-epoch-99-avg-1.int8.onnx', relativeDest: 'asr/decoder-epoch-99-avg-1.int8.onnx', expectedSize: 5165083, critical: true },
  { rawPath: 'models/asr/joiner-epoch-99-avg-1.onnx', relativeDest: 'asr/joiner-epoch-99-avg-1.onnx', expectedSize: 1033416, critical: false },
  { rawPath: 'models/asr/tokens.txt', relativeDest: 'asr/tokens.txt', expectedSize: 20628, critical: false },
  { rawPath: 'models/vad/silero_vad.onnx', relativeDest: 'vad/silero_vad.onnx', expectedSize: 643854, critical: false },
  { rawPath: 'models/tts/model.onnx', relativeDest: 'tts/model.onnx', expectedSize: 170429550, critical: false },
  { rawPath: 'models/tts/tokens.txt', relativeDest: 'tts/tokens.txt', expectedSize: 655, critical: false },
  { rawPath: 'models/tts/lexicon.txt', relativeDest: 'tts/lexicon.txt', expectedSize: 6837671, critical: false }
];

const TTS_DICT_FILES: BundledModelFile[] = [
  { rawPath: 'models/tts/dict/jieba.dict.utf8', relativeDest: 'tts/dict/jieba.dict.utf8', expectedSize: 5071204, critical: false },
  { rawPath: 'models/tts/dict/hmm_model.utf8', relativeDest: 'tts/dict/hmm_model.utf8', expectedSize: 519739, critical: false },
  { rawPath: 'models/tts/dict/user.dict.utf8', relativeDest: 'tts/dict/user.dict.utf8', expectedSize: 49, critical: false },
  { rawPath: 'models/tts/dict/idf.utf8', relativeDest: 'tts/dict/idf.utf8', expectedSize: 5998717, critical: false },
  { rawPath: 'models/tts/dict/stop_words.utf8', relativeDest: 'tts/dict/stop_words.utf8', expectedSize: 8974, critical: false },
  { rawPath: 'models/tts/dict/char_state_tab.utf8', relativeDest: 'tts/dict/char_state_tab.utf8', expectedSize: 0, critical: false },
  { rawPath: 'models/tts/dict/prob_emit.utf8', relativeDest: 'tts/dict/prob_emit.utf8', expectedSize: 0, critical: false },
  { rawPath: 'models/tts/dict/prob_start.utf8', relativeDest: 'tts/dict/prob_start.utf8', expectedSize: 0, critical: false },
  { rawPath: 'models/tts/dict/prob_trans.utf8', relativeDest: 'tts/dict/prob_trans.utf8', expectedSize: 0, critical: false }
];

export function isBundledModelFileValid(actualSize: number, expectedSize: number): boolean {
  return actualSize > 0 && (expectedSize <= 0 || actualSize === expectedSize);
}

export class SpeechModelInstaller {
  private readonly context: common.UIAbilityContext;
  private readonly rawfileContext?: Object;
  private readonly sandboxDir: string;

  constructor(context: common.UIAbilityContext, rawfileContext?: Object) {
    this.context = context;
    this.rawfileContext = rawfileContext;
    this.sandboxDir = context.filesDir + '/models';
  }

  async ensureInstalled(): Promise<string> {
    this.cleanupLegacyLlmFiles();
    if (this.rawfileContext) nativeLib.initRawfileMgmt(this.rawfileContext);

    let criticalReady: boolean = true;
    for (let i: number = 0; i < CORE_MODEL_FILES.length; i++) {
      const item: BundledModelFile = CORE_MODEL_FILES[i];
      const ok: boolean = await this.copyOne(item);
      if (!ok) {
        console.warn('OHOS_load: FAILED to copy ' + item.rawPath);
        if (item.critical) criticalReady = false;
      }
    }
    if (!criticalReady) throw new Error('核心模型文件复制失败');

    for (let i: number = 0; i < TTS_DICT_FILES.length; i++) {
      const item: BundledModelFile = TTS_DICT_FILES[i];
      const ok: boolean = await this.copyOne(item);
      if (!ok) console.warn('OHOS_TTS: dict skip: ' + item.rawPath);
    }
    console.info('OHOS_TTS: Melo dict ready');
    return this.sandboxDir;
  }

  private async copyOne(item: BundledModelFile): Promise<boolean> {
    const destPath: string = this.sandboxDir + '/' + item.relativeDest;
    const destDir: string = destPath.substring(0, destPath.lastIndexOf('/'));
    try { fileIo.mkdirSync(destDir, true); } catch (_) {}
    try {
      const existingSize: number = fileIo.statSync(destPath).size;
      if (isBundledModelFileValid(existingSize, item.expectedSize)) {
        console.info('OHOS_load: reuse verified model ' + item.rawPath + ' bytes=' + existingSize);
        return true;
      }
      console.warn('OHOS_load: replacing invalid model ' + item.rawPath +
        ' actual=' + existingSize + ' expected=' + item.expectedSize);
      fileIo.unlinkSync(destPath);
    } catch (_) {}

    try {
      const ok: boolean = nativeLib.copyRawFile(item.rawPath, destPath) as boolean;
      if (ok) return this.verifyCopiedFile(item, destPath);
    } catch (e) {
      console.warn('OHOS_load: copyRawFile exception: ' + String(e));
    }

    let file: fileIo.File | null = null;
    try {
      const data: Uint8Array = await this.context.resourceManager.getRawFileContent(item.rawPath);
      file = fileIo.openSync(destPath, fileIo.OpenMode.CREATE | fileIo.OpenMode.WRITE_ONLY |
        fileIo.OpenMode.TRUNC);
      fileIo.writeSync(file.fd, data);
      fileIo.closeSync(file);
      file = null;
      return this.verifyCopiedFile(item, destPath);
    } catch (e) {
      if (file !== null) {
        try { fileIo.closeSync(file); } catch (_) {}
      }
      console.warn('OHOS_load: ArkTS fallback failed: ' + String(e));
      return false;
    }
  }

  private verifyCopiedFile(item: BundledModelFile, destPath: string): boolean {
    try {
      const size: number = fileIo.statSync(destPath).size;
      if (!isBundledModelFileValid(size, item.expectedSize)) {
        console.error('OHOS_load: copied model size mismatch ' + item.rawPath);
        try { fileIo.unlinkSync(destPath); } catch (_) {}
        return false;
      }
      console.info('OHOS_load: model copied ' + item.rawPath + ' bytes=' + size);
      return true;
    } catch (_) {
      return false;
    }
  }

  private cleanupLegacyLlmFiles(): void {
    const legacyFiles: string[] = [
      this.sandboxDir + '/llm/model.int4.onnx',
      this.sandboxDir + '/llm/tokenizer.json'
    ];
    let freedBytes: number = 0;
    for (let i: number = 0; i < legacyFiles.length; i++) {
      try {
        freedBytes += fileIo.statSync(legacyFiles[i]).size;
        fileIo.unlinkSync(legacyFiles[i]);
      } catch (_) {}
    }
    if (freedBytes > 0) console.info('OHOS_LLM: legacy model cleanup freed bytes=' + freedBytes);
  }
}
