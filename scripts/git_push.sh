#!/bin/bash
set -e
cd "$(dirname "$0")/.."
# Stage all tracked files respecting .gitignore
git add -A
# Commit
git commit -m "Initial commit: OHOS Native AI Engine

基于 HarmonyOS (API 22) + sherpa-onnx 1.13.2 + ONNX Runtime 1.18.0
的全双工语音对话助手，ASR -> VAD -> LLM -> TTS 完整闭环。

- 全双工 6 状态音频状态机 (C++ 事件驱动)
- ASR: sherpa-onnx Zipformer Transducer INT8
- VAD: Silero VAD ONNX
- TTS: VITS Melo ONNX + Supertonic
- AEC: NLMS 自适应滤波器 (4096 taps)
- LLM: Ollama / DeepSeek / OpenAI 多后端
- 跨平台: OHOS NAPI + Windows DLL + Qt6 UI
- 硬件加速: MediaCodec / DXVA2"
# Push to origin
git push -u origin main
