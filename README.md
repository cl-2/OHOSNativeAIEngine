# Native AI Engine — 跨平台音视频加速 + 实时语音助手

[![CI](https://github.com/user/repo/actions/workflows/build.yml/badge.svg)](https://github.com/user/repo/actions/workflows/build.yml)
![HarmonyOS](https://img.shields.io/badge/HarmonyOS-✓-success)
![Windows](https://img.shields.io/badge/Windows-✓-blue)
![C++17](https://img.shields.io/badge/C%2B%2B-17-purple)

## 概述

一个跨平台的 **AI 引擎 + 硬件加速适配层**，支持 HarmonyOS 和 Windows 双平台。集成 **Wake-Word 检测 → ASR 语音识别 → TTS 语音合成** 全链路，并附带硬件编解码基准测试工具。

### 核心特性

| 特性 | 状态 | 说明 |
|------|------|------|
| 🎤 流式 ASR | ✅ | sherpa-onnx + silero VAD，RTF < 0.3 |
| 🗣️ Wake-Word | ✅ | 关键词唤醒，延迟 ≤150ms |
| 🔊 本地 TTS | ✅ | VITS 模型，合成延迟 ≤200ms |
| 🏎️ 硬件加速 | ✅ | MediaCodec (OHOS) / DXVA2 (Windows) |
| 📊 性能监控 | ✅ | 实时 RTF/延迟仪表盘 + 自动降级 |
| 📴 离线模式 | ✅ | RTF > 0.3 自动切换轻量模型 |
| 🔄 跨平台 | ✅ | 统一 CMake + NAPI 接口 |

## 项目结构

```
├── CMakeLists.txt              # 顶层跨平台 CMake
├── cmake/
│   ├── toolchain-harmonyos.cmake  # OHOS 交叉编译工具链
│   └── toolchain-windows.cmake    # Windows MSVC 工具链
├── entry/src/main/
│   ├── cpp/
│   │   ├── napi_init.cpp          # NAPI 入口 + ASR/TTS 实现
│   │   ├── RingBuffer.h           # 无锁环形缓冲区
│   │   ├── hardware/
│   │   │   ├── IHardwareCodec.h       # 硬件加速抽象接口
│   │   │   ├── MediaCodecAdapter.*    # OHOS MediaCodec 适配器
│   │   │   ├── DXVA2Decoder.*        # Windows DXVA2 适配器
│   │   │   ├── MediaCodecAdapter_napi.*  # NAPI 绑定
│   │   │   └── CodecBenchmark.*      # 硬件基准测试
│   │   └── CMakeLists.txt
│   ├── ets/
│   │   ├── pages/Index.ets         # 主界面（千问风格 UI）
│   │   ├── pages/MessageBubble.ets # 对话气泡组件
│   │   └── workers/
│   │       ├── AsrWorker.ets       # ASR Worker 线程
│   │       └── TtsWorker.ets       # TTS Worker 线程
│   └── resources/                  # 模型文件
├── scripts/
│   ├── build-harmony.bat
│   ├── build-windows.bat
│   └── build-all.bat
├── .github/workflows/build.yml    # CI/CD
└── docs/
    └── setup-ohos-ci.md
```

## 快速开始

### 前置条件

- **DevEco Studio** (HarmonyOS)
- **Visual Studio 2022** (Windows)
- **CMake ≥ 3.16**
- **OHOS Native SDK** (通过 DevEco Studio安装)

### HarmonyOS 构建

```bash
# 方式1: 使用 DevEco Studio 直接打开项目，同步并构建
# 方式2: 命令行
cd entry/src/main/cpp
cmake -B build/ohos \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-harmonyos.cmake \
  -DPLATFORM=OHOS
cmake --build build/ohos
```

### Windows 构建

```powershell
# 从 "Developer Command Prompt for VS 2022" 运行
cmake -B build/windows -G "Visual Studio 17 2022" -A x64 -DPLATFORM=WINDOWS
cmake --build build/windows --config Release
```

### 一键全平台构建

```bash
scripts\build-all.bat
```

## NAPI 接口

```typescript
// === ASR ===
startAssistant(asrModelDir, vadModelPath, kwsModelPath, asrCallback);
feedAudio(buffer: ArrayBuffer);
stopAssistant();

// === TTS ===
startStreamingTtsWithSpeed(text, modelDir, speed, callback);
stopTts();

// === 性能监控 ===
setPerfCallback(perfCallback);

// === 硬件加速 (HAL) ===
createHardwareCodec(): HardwareCodec;
initHardwareCodec(codec, mimeType, isEncoder): boolean;
codecQueueInput(codec, data, pts): boolean;
codecDequeueOutput(codec): CodecOutputData;
releaseHardwareCodec(codec): void;

// === 基准测试 ===
runCodecBenchmark(mimeType, testDurationMs?): CodecBenchmarkResult;
```

## 性能指标

| 指标 | 目标值 | 实测值 |
|------|--------|--------|
| Wake-Word 延迟 | ≤150ms | ~120ms |
| ASR 实时因子 (RTF) | < 0.3 | ~0.15 |
| TTS 合成延迟 | ≤200ms | ~180ms |
| 硬件解码吞吐 | 30fps+ | 60fps (MediaCodec) |
| CPU 占用 (解码) | < 15% | ~8% |

## 模型文件

ASR/TTS 模型文件需放置在 `entry/src/main/resources/rawfile/models/` 目录下：

```
models/
├── asr/                    # 主 ASR 模型 (zipformer)
│   ├── encoder-epoch-99-avg-1.int8.onnx
│   ├── decoder-epoch-99-avg-1.int8.onnx
│   ├── joiner-epoch-99-avg-1.onnx
│   └── tokens.txt
├── asr_light/              # 轻量 ASR 模型 (降级用)
│   ├── encoder.onnx
│   ├── decoder.onnx
│   └── tokens.txt
├── vad/
│   └── silero_vad.onnx     # VAD 模型
├── tts/
│   ├── model.onnx          # VITS 语音合成
│   ├── tokens.txt
│   └── lexicon.txt
└── kws/
    └── keywords.txt        # 唤醒词配置
```

## 许可证

MIT License
