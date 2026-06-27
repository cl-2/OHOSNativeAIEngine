#ifndef ENGINE_BRIDGE_H
#define ENGINE_BRIDGE_H

/**
 * @brief native_ai.dll 动态加载桥接层
 *
 * 在运行时加载 native_ai.dll，导出所有 C API 函数指针。
 * 如果 DLL 不存在或加载失败，所有调用安全返回默认值。
 */

#include <QString>
#include <QLibrary>
#include <QDebug>
#include <functional>

// ========== 回调类型定义 (与 native_ai_api.h 一致) ==========
using AsrResultCallback  = void (*)(const char* text, bool isFinal);
using PerfCallback       = void (*)(double currentRtf, double avgRtf, double maxRtf,
                                    long long latencyMs, long long vadLatencyMs, bool needsDegradation);
using TtsAudioCallback   = void (*)(const float* audio, int numSamples, int sampleRate, bool isLast);
using LlmResponseCallback = void (*)(const char* response);
using LlmStreamCallback  = void (*)(const char* chunk);
using LlmErrorCallback   = void (*)(const char* error);

class EngineBridge {
public:
    static EngineBridge& instance();

    // 生命周期
    bool load(const QString& dllPath = QString());
    void unload();
    bool isLoaded() const { return m_loaded; }
    QString lastError() const { return m_lastError; }

    // ----- 包装的 API 函数 -----
    const char* getVersion();

    bool init(const char* modelsDir);
    void destroy();

    // ASR
    bool startASR();
    void feedAudio(const short* pcm, int numSamples);
    void stopASR();
    bool isAsrRunning();
    void setAsrCallback(AsrResultCallback cb);
    void setPerfCallback(PerfCallback cb);

    // TTS
    bool startTTS(const char* text, float speed);
    void stopTTS();
    bool isTtsBusy();
    void setTtsCallback(TtsAudioCallback cb);

    // Supertonic TTS
    bool initSupertonic(const char* modelDir, const char* voiceStyle);
    bool setSupertonicVoice(const char* voiceName);
    int  getSupertonicVoiceCount();
    const char* getSupertonicVoiceName(int index);
    bool supertonicIsReady();

    // LLM
    void sendLlmMessage(const char* text);
    void sendLlmMessageStream(const char* text);
    void stopLlm();
    void clearLlmHistory();
    void setLlmConfig(const char* apiUrl, const char* apiKey, const char* modelName);
    void setLlmCallbacks(LlmResponseCallback onResponse, LlmStreamCallback onStream, LlmErrorCallback onError);

    // Audio capture (microphone)
    bool startCapture(int sampleRate = 16000);
    void stopCapture();
    bool isCapturing();

    // Audio playback (TTS)
    void playAudio(const float* audio, int numSamples, int sampleRate);
    void stopPlayback();
    bool isPlaying();

    // Full duplex
    void startFullDuplex();
    void stopFullDuplex();
    const char* getState();

    // Hardware
    void* createHardwareCodec();
    bool  initHardwareCodec(void* codec, const char* mimeType, bool isEncoder);
    bool  codecQueueInput(void* codec, const unsigned char* data, int size, long long pts);
    void* codecDequeueOutput(void* codec, int* outSize, long long* outPts);
    void  releaseHardwareCodec(void* codec);

    // Util
    void setLogLevel(int level);

private:
    EngineBridge() = default;
    ~EngineBridge() { unload(); }
    EngineBridge(const EngineBridge&) = delete;
    EngineBridge& operator=(const EngineBridge&) = delete;

    template<typename Func>
    Func resolve(const char* symbol) {
        if (!m_lib) return nullptr;
        return reinterpret_cast<Func>(m_lib->resolve(symbol));
    }

    QLibrary* m_lib = nullptr;
    bool m_loaded = false;
    QString m_lastError;
};

#endif // ENGINE_BRIDGE_H
