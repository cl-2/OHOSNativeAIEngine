#ifndef TTS_ENGINE_H
#define TTS_ENGINE_H

#include <string>
#include <functional>
#include <memory>

/**
 * @brief TTS 引擎纯虚接口
 * 
 * 支持 sherpa-onnx VITS 和 Supertonic 3 两种后端。
 * Windows 和 HarmonyOS 共用。
 */
class TtsEngine {
public:
    virtual ~TtsEngine() = default;

    /// 初始化 TTS 引擎 (sherpa-onnx VITS)
    virtual bool Init(const std::string& modelDir) = 0;

    /// 合成语音 (完整文本)
    virtual bool Synthesize(const std::string& text, float speed) = 0;

    /// 停止合成
    virtual void Stop() = 0;

    /// 是否正在合成
    virtual bool IsBusy() const = 0;

    // ========== 回调 ==========

    /// 音频数据回调 (float32 PCM)
    /// audio: PCM 数据, numSamples: 采样数, sampleRate: 采样率, isLast: 最后一块
    std::function<void(const float* audio, int numSamples, int sampleRate, bool isLast)> OnAudio;

    /// 进度回调 (0.0 ~ 1.0)
    std::function<void(float progress)> OnProgress;
};

/**
 * @brief Supertonic TTS 引擎接口
 * 
 * Supertonic 3 提供更自然的语音合成。
 * 作为 TtsEngine 的替代/升级后端。
 */
class SupertonicTtsEngine {
public:
    virtual ~SupertonicTtsEngine() = default;

    virtual bool Init(const std::string& modelDir, const std::string& voiceStyle) = 0;
    virtual bool Synthesize(const std::string& text, const std::string& lang,
                            float speed, int totalSteps) = 0;
    virtual void Stop() = 0;
    virtual bool IsBusy() const = 0;
    virtual bool SetVoiceStyle(const std::string& voiceName) = 0;
    virtual std::vector<std::string> GetAvailableVoices() const = 0;

    std::function<void(const float* audio, int numSamples, int sampleRate, bool isLast)> OnAudio;
};

#endif // TTS_ENGINE_H
