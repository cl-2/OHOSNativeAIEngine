// VAD 辅助模块 - 简化版，避免 ArkTS 顶层变量初始化问题

let silenceDuration: number = 0;
const SILENCE_THRESHOLD: number = 1.0; // 超过1秒静音则打断

// 简易能量检测 VAD（不依赖 sherpa-onnx Vad 模块）
function isLikelySilence(pcm: ArrayBuffer): boolean {
  const int16 = new Int16Array(pcm);
  let energy: number = 0;
  for (let i = 0; i < int16.length; i++) {
    energy += Math.abs(int16[i]);
  }
  const avgEnergy: number = energy / int16.length;
  // 阈值可调，低于 300 认为是静音
  return avgEnergy < 300;
}

export function checkVadAndUpdateSilence(pcm: ArrayBuffer): boolean {
  const isSilent: boolean = isLikelySilence(pcm);
  const sampleCount: number = pcm.byteLength / 2; // int16 = 2 bytes
  if (isSilent) {
    silenceDuration += sampleCount / 16000;
  } else {
    silenceDuration = 0;
  }
  return silenceDuration >= SILENCE_THRESHOLD;
}

export function resetVadSilence(): void {
  silenceDuration = 0;
}
