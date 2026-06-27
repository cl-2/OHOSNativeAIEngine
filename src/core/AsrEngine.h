#ifndef ASR_ENGINE_H
#define ASR_ENGINE_H

#include <string>
#include <functional>
#include <memory>

/**
 * @brief ASR 引擎纯虚接口
 * 
 * 抽象语音识别引擎，支持 sherpa-onnx 实现。
 * Windows 和 HarmonyOS 共用同一接口。
 */
class AsrEngine {
public:
    virtual ~AsrEngine() = default;

    /// 初始化 ASR 引擎
    virtual bool Init(const std::string& modelDir,
                      const std::string& vadModel,
                      const std::string& kwsModel) = 0;

    /// 启动 ASR (开始后台解码线程)
    virtual bool Start() = 0;

    /// 喂入音频数据 (float32 PCM, 16kHz)
    virtual void FeedAudio(const float* samples, int numSamples) = 0;

    /// 停止 ASR
    virtual void Stop() = 0;

    /// 是否正在运行
    virtual bool IsRunning() const = 0;

    /// 重置识别器 (新对话)
    virtual void Reset() = 0;

    // ========== 回调 ==========

    /// ASR 识别结果回调
    std::function<void(const std::string& text, bool isFinal)> OnResult;

    /// 性能指标回调 (每500ms)
    std::function<void(double currentRtf, double avgRtf, double maxRtf,
                       int64_t latencyMs, int64_t vadLatencyMs,
                       bool needsDegradation)> OnPerfMetrics;

    /// 唤醒词检测回调
    std::function<void(const std::string& keyword)> OnWakeWord;
};

#endif // ASR_ENGINE_H
