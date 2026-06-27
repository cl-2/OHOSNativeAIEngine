#include "native_ai_api.h"
#include "Logger.h"
#include "HttpLlmClient.h"

#include <mutex>
#include <cstring>
#include <vector>
#include "WasapiCapture.h"
#include "WasapiRender.h"

// ============================================================
// native_ai.dll — DLL 主入口 + 全局引擎实例
// ============================================================

// Windows: ASR/TTS 引擎 (需要 sherpa-onnx SDK)
// 通过 -DSHERPA_ONNX_DIR=... 在 CMake 中启用
#ifdef HAS_SHERPA_ONNX
#include "SherpaAsrEngine.h"
#include "SherpaTtsEngine.h"
#endif

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        LOGI("native_ai.dll loaded");
        break;
    case DLL_PROCESS_DETACH:
        LOGI("native_ai.dll unloaded");
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}

// ============================================================
// 全局引擎实例 (延迟初始化)
// ============================================================

static std::mutex g_mutex;
static bool g_initialized = false;
static std::string g_modelsDir;

static HttpLlmClient*   g_llmClient = nullptr;
static WasapiCapture*   g_capture = nullptr;
static WasapiRender*    g_render = nullptr;

#ifdef HAS_SHERPA_ONNX
static SherpaAsrEngine* g_asrEngine = nullptr;
static SherpaTtsEngine* g_ttsEngine = nullptr;
#endif

// 回调指针
static AsrResultCallback g_asrCb = nullptr;
static PerfCallback       g_perfCb = nullptr;
static TtsAudioCallback   g_ttsCb = nullptr;
static LlmResponseCallback g_llmResponseCb = nullptr;
static LlmStreamCallback   g_llmStreamCb = nullptr;
static LlmErrorCallback    g_llmErrorCb = nullptr;

// ============================================================
// 辅助函数
// ============================================================

static void EnsureEngines() {
    if (!g_llmClient) g_llmClient = new HttpLlmClient();
    if (!g_capture) {
        g_capture = new WasapiCapture();
        g_capture->OnAudioData = [](const int16_t* pcm, int numSamples) {
            // 直接喂给 ASR 引擎
#ifdef HAS_SHERPA_ONNX
            if (g_asrEngine && g_asrEngine->IsRunning()) {
                AI_FeedAudio(pcm, numSamples);
            }
#endif
        };
    }
    if (!g_render) g_render = new WasapiRender();
#ifdef HAS_SHERPA_ONNX
    if (!g_asrEngine) g_asrEngine = new SherpaAsrEngine();
    if (!g_ttsEngine) g_ttsEngine = new SherpaTtsEngine();
#endif
}

// ============================================================
// API 实现
// ============================================================

NATIVE_API const char* AI_GetVersion() {
    return "Native AI Engine v1.0.0 (Windows)";
}

NATIVE_API bool AI_Init(const char* modelsDir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return true;

    g_modelsDir = modelsDir;
    EnsureEngines();

#ifdef HAS_SHERPA_ONNX
    LOGI("AI_Init: modelsDir=%s", modelsDir);
    // 初始化 ASR
    std::string asrDir = std::string(modelsDir) + "/asr";
    std::string vadPath = std::string(modelsDir) + "/vad/silero_vad.onnx";
    if (g_asrEngine->Init(asrDir, vadPath, "")) {
        LOGI("ASR engine initialized");
    } else {
        LOGW("ASR engine init failed (models may be missing)");
    }
    // 初始化 TTS
    std::string ttsDir = std::string(modelsDir) + "/tts";
    if (g_ttsEngine->Init(ttsDir)) {
        LOGI("TTS engine initialized");
    } else {
        LOGW("TTS engine init failed");
    }
#else
    LOGI("AI_Init: modelsDir=%s (sherpa-onnx SDK not available)", modelsDir);
#endif
    // 初始化音频播放
    if (g_render && !g_render->IsPlaying()) {
        g_render->Init(44100);
    }
    g_initialized = true;
    return true;
}

NATIVE_API void AI_Destroy() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_initialized = false;
#ifdef HAS_SHERPA_ONNX
    delete g_asrEngine;  g_asrEngine = nullptr;
    delete g_ttsEngine;  g_ttsEngine = nullptr;
#endif
    if (g_capture) { g_capture->Stop(); delete g_capture; g_capture = nullptr; }
    if (g_render) { g_render->Stop(); delete g_render; g_render = nullptr; }
    delete g_llmClient;  g_llmClient = nullptr;
    LOGI("AI_Destroy: all engines released");
}

// ========== ASR ==========

NATIVE_API bool AI_StartASR() {
#ifdef HAS_SHERPA_ONNX
    if (!g_asrEngine) return false;
    return g_asrEngine->Start();
#else
    LOGW("AI_StartASR: not available (sherpa-onnx SDK required)");
    return false;
#endif
}

NATIVE_API void AI_FeedAudio(const short* pcm, int numSamples) {
#ifdef HAS_SHERPA_ONNX
    if (!g_asrEngine || !g_asrEngine->IsRunning()) return;
    std::vector<float> floatSamples(numSamples);
    for (int i = 0; i < numSamples; i++) {
        floatSamples[i] = (float)pcm[i] / 32768.0f;
    }
    g_asrEngine->FeedAudio(floatSamples.data(), numSamples);
#endif
}

NATIVE_API void AI_StopASR() {
#ifdef HAS_SHERPA_ONNX
    if (g_asrEngine) g_asrEngine->Stop();
#endif
}

NATIVE_API bool AI_IsAsrRunning() {
#ifdef HAS_SHERPA_ONNX
    return g_asrEngine ? g_asrEngine->IsRunning() : false;
#else
    return false;
#endif
}

NATIVE_API void AI_SetAsrCallback(AsrResultCallback cb) {
    g_asrCb = cb;
#ifdef HAS_SHERPA_ONNX
    if (g_asrEngine) {
        g_asrEngine->OnResult = [](const std::string& text, bool isFinal) {
            if (g_asrCb) g_asrCb(text.c_str(), isFinal);
        };
        g_asrEngine->OnPerfMetrics = [](double rtf, double avg, double max,
                                         int64_t lat, int64_t vad, bool deg) {
            if (g_perfCb) g_perfCb(rtf, avg, max, lat, vad, deg);
        };
    }
#endif
}

NATIVE_API void AI_SetPerfCallback(PerfCallback cb) {
    g_perfCb = cb;
}

// ========== TTS ==========

NATIVE_API bool AI_StartTTS(const char* text, float speed) {
    LOGI("AI_StartTTS called: text='%s'", text ? text : "(null)");
#ifdef HAS_SHERPA_ONNX
    if (!g_ttsEngine) { LOGW("AI_StartTTS: g_ttsEngine null"); return false; }
    bool ret = g_ttsEngine->Synthesize(text, speed);
    LOGI("AI_StartTTS: returned %s", ret ? "true" : "false");
    return ret;
#else
    LOGW("AI_StartTTS: not available (sherpa-onnx SDK required)");
    return false;
#endif
}

NATIVE_API void AI_StopTTS() {
#ifdef HAS_SHERPA_ONNX
    if (g_ttsEngine) g_ttsEngine->Stop();
#endif
}

NATIVE_API bool AI_IsTtsBusy() {
#ifdef HAS_SHERPA_ONNX
    return g_ttsEngine ? g_ttsEngine->IsBusy() : false;
#else
    return false;
#endif
}

NATIVE_API void AI_SetTtsCallback(TtsAudioCallback cb) {
    g_ttsCb = cb;
#ifdef HAS_SHERPA_ONNX
    if (g_ttsEngine) {
        g_ttsEngine->OnAudio = [](const float* audio, int n, int sr, bool last) {
            // 播放到扬声器 (如果还没启动则自动启动)
            if (g_render) {
                if (!g_render->IsPlaying()) {
                    g_render->Init(sr > 0 ? sr : 44100);
                    g_render->Start();
                }
                g_render->EnqueueAudio(audio, n, sr);
            }
            // 通知 UI 回调
            if (g_ttsCb) g_ttsCb(audio, n, sr, last);
        };
    }
#endif
}

// ========== Supertonic TTS (存根，需要链接 SupertonicTtsEngine) ==========

NATIVE_API bool AI_InitSupertonic(const char* modelDir, const char* voiceStyle) {
    LOGI("AI_InitSupertonic: %s, voice=%s (stub)", modelDir, voiceStyle);
    return false;  // TODO: 链接 SupertonicTtsEngine
}

NATIVE_API bool AI_SetSupertonicVoice(const char* voiceName) {
    return false;
}

NATIVE_API int AI_GetSupertonicVoiceCount() {
    return 0;
}

NATIVE_API const char* AI_GetSupertonicVoiceName(int index) {
    return "";
}

NATIVE_API bool AI_SupertonicIsReady() {
    return false;
}

// ========== 音频捕获 ==========

NATIVE_API bool AI_StartCapture(int sampleRate) {
    if (!g_capture) return false;
    if (g_capture->IsCapturing()) return true;
    if (!g_capture->Init(sampleRate > 0 ? sampleRate : 16000)) {
        LOGE("AI_StartCapture: Init failed");
        return false;
    }
    return g_capture->Start();
}

NATIVE_API void AI_StopCapture() {
    if (g_capture) g_capture->Stop();
}

NATIVE_API bool AI_IsCapturing() {
    return g_capture ? g_capture->IsCapturing() : false;
}

// ========== 音频播放 ==========

NATIVE_API void AI_PlayAudio(const float* audio, int numSamples, int sampleRate) {
    if (g_render) {
        if (!g_render->IsPlaying()) {
            g_render->Init(sampleRate > 0 ? sampleRate : 44100);
            g_render->Start();
        }
        g_render->EnqueueAudio(audio, numSamples, sampleRate);
    }
}

NATIVE_API void AI_StopPlayback() {
    if (g_render) {
        g_render->Flush();
        g_render->Stop();
    }
}

NATIVE_API bool AI_IsPlaying() {
    return g_render ? g_render->IsPlaying() : false;
}

// ========== LLM ==========

NATIVE_API void AI_SendLlmMessage(const char* text) {
    if (g_llmClient) g_llmClient->SendMessage(text);
}

NATIVE_API void AI_SendLlmMessageStream(const char* text) {
    if (g_llmClient) g_llmClient->SendMessageStream(text);
}

NATIVE_API void AI_StopLlm() {
    if (g_llmClient) g_llmClient->Stop();
}

NATIVE_API void AI_ClearLlmHistory() {
    if (g_llmClient) g_llmClient->ClearHistory();
}

NATIVE_API void AI_SetLlmConfig(const char* apiUrl, const char* apiKey, const char* modelName) {
    if (g_llmClient) {
        LlmConfig config;
        config.apiUrl = apiUrl;
        config.apiKey = apiKey;
        config.modelName = modelName;
        g_llmClient->Init(config);
    }
}

NATIVE_API void AI_SetLlmCallbacks(LlmResponseCallback onResponse,
                                    LlmStreamCallback onStream,
                                    LlmErrorCallback onError) {
    g_llmResponseCb = onResponse;
    g_llmStreamCb = onStream;
    g_llmErrorCb = onError;

    if (g_llmClient) {
        g_llmClient->OnResponse = [](const std::string& resp) {
            if (g_llmResponseCb) g_llmResponseCb(resp.c_str());
        };
        g_llmClient->OnStreamChunk = [](const std::string& chunk) {
            if (g_llmStreamCb) g_llmStreamCb(chunk.c_str());
        };
        g_llmClient->OnError = [](const std::string& err) {
            if (g_llmErrorCb) g_llmErrorCb(err.c_str());
        };
    }
}

// ========== 全双工 (存根) ==========

NATIVE_API void AI_StartFullDuplex() {
    LOGI("AI_StartFullDuplex (stub)");
}

NATIVE_API void AI_StopFullDuplex() {
    LOGI("AI_StopFullDuplex (stub)");
}

NATIVE_API const char* AI_GetState() {
    return "IDLE";
}

// ========== 硬件加速 (存根) ==========

NATIVE_API void* AI_CreateHardwareCodec() {
    return nullptr;
}

NATIVE_API bool AI_InitHardwareCodec(void* codec, const char* mimeType, bool isEncoder) {
    return false;
}

NATIVE_API bool AI_CodecQueueInput(void* codec, const unsigned char* data, int size, long long pts) {
    return false;
}

NATIVE_API void* AI_CodecDequeueOutput(void* codec, int* outSize, long long* outPts) {
    return nullptr;
}

NATIVE_API void AI_ReleaseHardwareCodec(void* codec) {
}

// ========== 工具 ==========

NATIVE_API void AI_SetLogLevel(int level) {
    LOGI("AI_SetLogLevel: %d", level);
}
