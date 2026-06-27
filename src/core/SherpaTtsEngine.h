#ifndef SHERPA_TTS_ENGINE_H
#define SHERPA_TTS_ENGINE_H

#include "TtsEngine.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

// 前向声明
struct SherpaOnnxOfflineTts;

/**
 * @brief 基于 sherpa-onnx VITS 的 TTS 引擎实现
 * 
 * 从 napi_init.cpp 提取的 TTS 核心逻辑。
 * 支持语速调节、流式输出。
 */
class SherpaTtsEngine : public TtsEngine {
public:
    SherpaTtsEngine();
    ~SherpaTtsEngine() override;

    bool Init(const std::string& modelDir) override;
    bool Synthesize(const std::string& text, float speed) override;
    void Stop() override;
    bool IsBusy() const override;

private:
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_stopping{false};
    std::mutex m_mutex;
    std::string m_modelDir;

    // 后台合成线程
    std::thread m_synthThread;
    void SynthThreadProc(const std::string& text, float speed);

    // 检查文件存在
    static bool FileExists(const std::string& path);
};

#endif // SHERPA_TTS_ENGINE_H
