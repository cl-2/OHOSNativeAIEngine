#ifndef SHERPA_ASR_ENGINE_H
#define SHERPA_ASR_ENGINE_H

#include "AsrEngine.h"
#include "RingBuffer.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <chrono>

#ifdef _WIN32
// Windows 上使用动态加载，不需要编译时链接
struct SherpaOnnxOnlineRecognizer;
struct SherpaOnnxOnlineStream;
struct SherpaOnnxVoiceActivityDetector;
struct SherpaOnnxKeywordSpotter;
#else
// OHOS: 使用 sherpa-onnx C API
#include "sherpa-onnx/c-api.h"
#endif

/**
 * @brief 基于 sherpa-onnx 的 ASR 引擎实现
 * 
 * 从 napi_init.cpp 提取的 ASR 核心逻辑。
 * 支持 VAD、唤醒词、RTF 性能监控。
 */
class SherpaAsrEngine : public AsrEngine {
public:
    SherpaAsrEngine();
    ~SherpaAsrEngine() override;

    bool Init(const std::string& modelDir,
              const std::string& vadModel,
              const std::string& kwsModel) override;

    bool Start() override;
    void FeedAudio(const float* samples, int numSamples) override;
    void Stop() override;
    bool IsRunning() const override;
    void Reset() override;

private:
    // ========== sherpa-onnx 对象 ==========
    SherpaOnnxOnlineRecognizer* m_recognizer = nullptr;
    SherpaOnnxOnlineStream* m_stream = nullptr;
    SherpaOnnxVoiceActivityDetector* m_vad = nullptr;
    SherpaOnnxKeywordSpotter* m_keywordSpotter = nullptr;
    SherpaOnnxOnlineStream* m_keywordStream = nullptr;

    // ========== 状态 ==========
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_wakeWordDetected{false};
    std::mutex m_mutex;

    // ========== 环形缓冲区 ==========
    RingBuffer<float> m_ringBuffer{320000};  // 16kHz × 10秒

    // ========== 解码线程 ==========
    std::thread m_decodeThread;
    void DecodeLoop();

    // ========== 重复结果去重 ==========
    std::string m_lastText;

    // ========== RTF 性能监控 ==========
    struct RtfMetrics {
        std::atomic<double> currentRtf{0.0};
        std::atomic<double> avgRtf{0.0};
        std::atomic<double> maxRtf{0.0};
        std::atomic<int64_t> totalAudioMs{0};
        std::atomic<int64_t> totalDecodeMs{0};
        std::atomic<int64_t> lastLatencyMs{0};
        std::atomic<int64_t> vadLatencyMs{0};
        std::atomic<bool> needsDegradation{false};
        std::deque<double> rtfWindow;
        std::mutex windowMutex;

        void RecordRtf(double rtf);
        void Reset();
    };
    RtfMetrics m_rtfMetrics;

    // 性能推送间隔
    std::chrono::steady_clock::time_point m_lastPerfPush;
};

#endif // SHERPA_ASR_ENGINE_H
