#ifndef NATIVE_AI_API_H
#define NATIVE_AI_API_H

/**
 * @brief native_ai.dll 统一 C API
 * 
 * 供 Qt UI 或其他 Windows 客户端调用。
 * 替代 HarmonyOS 的 NAPI 接口。
 */

#ifdef NATIVE_AI_EXPORTS
#define NATIVE_API __declspec(dllexport)
#else
#define NATIVE_API __declspec(dllimport)
#endif

extern "C" {

// ========== 版本信息 ==========
NATIVE_API const char* AI_GetVersion();

// ========== 生命周期 ==========
NATIVE_API bool    AI_Init(const char* modelsDir);
NATIVE_API void    AI_Destroy();

// ========== ASR ==========
NATIVE_API bool    AI_StartASR();
NATIVE_API void    AI_FeedAudio(const short* pcm, int numSamples);
NATIVE_API void    AI_StopASR();
NATIVE_API bool    AI_IsAsrRunning();

// ASR 回调注册
typedef void (*AsrResultCallback)(const char* text, bool isFinal);
typedef void (*PerfCallback)(double currentRtf, double avgRtf, double maxRtf,
                             long long latencyMs, long long vadLatencyMs, bool needsDegradation);
NATIVE_API void    AI_SetAsrCallback(AsrResultCallback cb);
NATIVE_API void    AI_SetPerfCallback(PerfCallback cb);

// ========== TTS (sherpa-onnx VITS) ==========
NATIVE_API bool    AI_StartTTS(const char* text, float speed);
NATIVE_API void    AI_StopTTS();
NATIVE_API bool    AI_IsTtsBusy();

// TTS 回调
typedef void (*TtsAudioCallback)(const float* audio, int numSamples, int sampleRate, bool isLast);
NATIVE_API void    AI_SetTtsCallback(TtsAudioCallback cb);

// ========== Supertonic TTS ==========
NATIVE_API bool    AI_InitSupertonic(const char* modelDir, const char* voiceStyle);
NATIVE_API bool    AI_SetSupertonicVoice(const char* voiceName);
NATIVE_API int     AI_GetSupertonicVoiceCount();
NATIVE_API const char* AI_GetSupertonicVoiceName(int index);
NATIVE_API bool    AI_SupertonicIsReady();

// ========== LLM ==========
NATIVE_API void    AI_SendLlmMessage(const char* text);
NATIVE_API void    AI_SendLlmMessageStream(const char* text);
NATIVE_API void    AI_StopLlm();
NATIVE_API void    AI_ClearLlmHistory();
NATIVE_API void    AI_SetLlmConfig(const char* apiUrl, const char* apiKey, const char* modelName);

// LLM 回调
typedef void (*LlmResponseCallback)(const char* response);
typedef void (*LlmStreamCallback)(const char* chunk);
typedef void (*LlmErrorCallback)(const char* error);
NATIVE_API void    AI_SetLlmCallbacks(LlmResponseCallback onResponse,
                                       LlmStreamCallback onStream,
                                       LlmErrorCallback onError);

// ========== 音频捕获 (麦克风) ==========
NATIVE_API bool    AI_StartCapture(int sampleRate);
NATIVE_API void    AI_StopCapture();
NATIVE_API bool    AI_IsCapturing();

// ========== 音频播放 (TTS) ==========
NATIVE_API void    AI_PlayAudio(const float* audio, int numSamples, int sampleRate);
NATIVE_API void    AI_StopPlayback();
NATIVE_API bool    AI_IsPlaying();

// ========== 全双工 ==========
NATIVE_API void    AI_StartFullDuplex();
NATIVE_API void    AI_StopFullDuplex();
NATIVE_API const char* AI_GetState();

// ========== 硬件加速 ==========
NATIVE_API void*   AI_CreateHardwareCodec();
NATIVE_API bool    AI_InitHardwareCodec(void* codec, const char* mimeType, bool isEncoder);
NATIVE_API bool    AI_CodecQueueInput(void* codec, const unsigned char* data, int size, long long pts);
NATIVE_API void*   AI_CodecDequeueOutput(void* codec, int* outSize, long long* outPts);
NATIVE_API void    AI_ReleaseHardwareCodec(void* codec);

// ========== 工具 ==========
NATIVE_API void    AI_SetLogLevel(int level);

}  // extern "C"

#endif // NATIVE_AI_API_H
