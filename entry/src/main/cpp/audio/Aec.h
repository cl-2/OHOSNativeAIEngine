// Aec.h - 声学回声消除 (Acoustic Echo Cancellation)
// 基于 NLMS (Normalized Least Mean Squares) 自适应滤波器
// 用于全双工场景：TTS 播放时消除扬声器回声，保留人声
//
// 算法：
//   1. TTS 实际提交给 AudioRenderer 时记录参考信号（存到 FIFO）
//   2. 麦克风采集时，用参考信号估计回声路径
//   3. 从麦克风信号中减去估计的回声，得到纯净人声
//
// 使用方式：
//   - TTS 播放线程调用 AddTtsReference()
//   - 音频采集线程调用 ProcessMicAudio()
//   线程安全：内部使用 std::mutex 保护参考缓冲区

#pragma once

#include <cstdint>
#include <vector>
#include <atomic>
#include <mutex>

class AcousticEchoCanceller {
public:
    /**
     * @brief 构造函数
     * @param filterLength 滤波器阶数（tap数），推荐 4096 ≈ 256ms @16kHz
     * @param stepSize     NLMS 步长，0.1~0.5，默认 0.3
     */
    explicit AcousticEchoCanceller(size_t filterLength = 4096, float stepSize = 0.3f);
    ~AcousticEchoCanceller() = default;

    // 禁止拷贝
    AcousticEchoCanceller(const AcousticEchoCanceller&) = delete;
    AcousticEchoCanceller& operator=(const AcousticEchoCanceller&) = delete;

    // ========== 公开接口 ==========

    /// 记录已提交给 AudioRenderer 的 TTS 参考信号。
    /// ProcessMicAudio 会按采样顺序消费它，避免使用“最新生成音频”造成时序错位。
    /// @param samples 浮点 PCM 样本 [-1.0, 1.0]
    /// @param n       样本数
    void AddTtsReference(const float* samples, size_t n);

    /// 处理麦克风音频：原地消除回声
    /// @param micInOut 输入/输出缓冲区（原地修改）
    /// @param n        样本数
    /// @return 实际处理的样本数
    size_t ProcessMicAudio(float* micInOut, size_t n);

    /// 重置滤波器状态（例如回声路径变化时）
    void Reset();

    // ========== 状态查询 ==========

    bool IsActive() const { return m_active; }
    void SetActive(bool active) { m_active = active; }

    /// 滤波器是否已收敛（有足够参考数据且 ERLE > 5dB）
    bool IsConverged() const { return m_converged; }

    /// 回声返回损耗增强 (dB)，越大说明消回声效果越好
    float GetErleDb() const { return m_erleDb; }

    /// 滤波器是否有足够的参考数据可以开始工作
    bool HasEnoughReference() const;

private:
    /// 单样本 NLMS 处理
    float ProcessSample(float micSample);

    // ========== 参数 ==========
    size_t m_filterLength;   // 滤波器阶数
    float  m_stepSize;       // NLMS 步长 μ
    float  m_regularization; // 正则化 ε，防止除零

    // ========== 滤波器系数 ==========
    std::vector<float> m_w;  // 自适应滤波器权重 (冲击响应估计)

    // ========== 参考信号 FIFO 与历史帧 ==========
    // 参考 FIFO 保存已经提交播放的 PCM；历史帧保存与麦克风时间轴对齐的参考信号。
    std::vector<float> m_refFifo;
    std::vector<float> m_refHistory;
    size_t             m_refCapacity;
    size_t             m_refWritePos;
    size_t             m_refReadPos;
    size_t             m_refAvailable;
    size_t             m_refHistoryPos;
    size_t             m_refHistoryCount;
    mutable std::mutex m_refMutex;     // 保护参考缓冲区的并发访问

    // ========== 状态 ==========
    std::atomic<bool>  m_active{true};
    std::atomic<bool>  m_converged{false};
    int64_t            m_samplesProcessed{0};

    // ========== 统计 ==========
    float m_runningAvgError; // 误差信号（残差）的指数移动平均
    float m_runningAvgEcho;  // 估计回声的指数移动平均
    float m_erleDb;          // 回声返回损耗增强 (dB)
};
