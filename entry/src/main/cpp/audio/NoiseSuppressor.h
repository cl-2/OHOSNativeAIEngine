#ifndef NOISE_SUPPRESSOR_H
#define NOISE_SUPPRESSOR_H

#include <cstdint>
#include <vector>
#include <memory>

/**
 * @brief 噪声抑制器 — RNNoise 封装
 *
 * 基于 RNNoise (https://github.com/xiph/rnnoise) 的实时语音增强。
 * 使用训练好的 RNN 模型做噪声抑制，有效降低风扇/空调/环境背景噪声。
 *
 * 用法：
 *   NoiseSuppressor ns;
 *   ns.Process(floatSamples.data(), sampleCount);
 *
 * 帧大小：480 samples (30ms @ 16kHz)
 * 内部自动处理分帧和残帧缓冲。
 *
 * 插入位置：DC 阻塞滤波之后，AEC 之前
 */

// RNNoise 前向声明
typedef struct DenoiseState DenoiseState;

class NoiseSuppressor {
public:
    NoiseSuppressor();
    ~NoiseSuppressor();

    NoiseSuppressor(const NoiseSuppressor&) = delete;
    NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

    /// 初始化（加载模型，首次调用自动初始化）
    bool Init();

    /// 处理音频数据（in-place 修改）
    /// @param samples 输入输出音频数据 [-1.0, 1.0]
    /// @param n       样本数
    void Process(float* samples, size_t n);

    /// 获取最后一帧的 VAD 概率 [0, 1]
    float GetLastVadProb() const { return m_lastVadProb; }

    /// 是否已初始化
    bool IsInitialized() const { return m_initialized; }

    /// 重置状态
    void Reset();

    /// 获取帧大小（480 samples = 30ms @ 16kHz）
    static constexpr size_t kFrameSize = 480;

private:
    /// 处理一个完整的帧
    void ProcessFrame(const float* in, float* out);

    // 状态
    bool m_initialized = false;
    DenoiseState* m_rnnState = nullptr;

    // 残帧缓冲（不足一帧的音频累积到这里）
    std::vector<float> m_tailBuffer;

    // 指标
    float m_lastVadProb = 0.0f;
};

#endif // NOISE_SUPPRESSOR_H
