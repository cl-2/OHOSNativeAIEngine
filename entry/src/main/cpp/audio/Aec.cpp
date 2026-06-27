// Aec.cpp - NLMS 自适应滤波器实现
//
// Normalized Least Mean Squares (NLMS) 算法：
//
// 对每个采样点 n:
//   1. 构建参考帧: x[n] = [ref[t-1], ref[t-2], ..., ref[t-N]]
//   2. 估计回声:    y[n] = w^T · x[n]
//   3. 误差信号:    e[n] = mic[n] - y[n]  (即消除回声后的人声)
//   4. 更新权重:    w = w + μ · e[n] · x[n] / (||x[n]||² + ε)
//
// 其中 N = filterLength (滤波器阶数)
//      μ = stepSize (步长，控制收敛速度与稳定性)
//      ε = regularization (正则化，防止除零)

#include "Aec.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

// ============================================================
// 构造 / 析构
// ============================================================

AcousticEchoCanceller::AcousticEchoCanceller(size_t filterLength, float stepSize)
    : m_filterLength(filterLength)
    , m_stepSize(stepSize)
    , m_regularization(1.0e-6f)
    , m_w(filterLength, 0.0f)
    , m_refCapacity(filterLength * 4)  // 4倍容量，保证历史数据充足
    , m_refBuffer(filterLength * 4, 0.0f)  // 用 filterLength*4 而非 m_refCapacity，避免成员初始化顺序坑
    , m_refWritePos(0)
    , m_samplesProcessed(0)
    , m_runningAvgError(1.0f)
    , m_runningAvgEcho(1.0f)
    , m_erleDb(0.0f)
    , m_active(true)  // 默认激活 AEC
{
}

// ============================================================
// 公开接口
// ============================================================

void AcousticEchoCanceller::AddTtsReference(const float* samples, size_t n) {
    if (!samples || n == 0) return;

    std::lock_guard<std::mutex> lock(m_refMutex);
    for (size_t i = 0; i < n; i++) {
        m_refBuffer[m_refWritePos] = samples[i];
        m_refWritePos = (m_refWritePos + 1) % m_refCapacity;
    }
}

size_t AcousticEchoCanceller::ProcessMicAudio(float* micInOut, size_t n) {
    if (!m_active || !micInOut || n == 0) {
        return n;
    }

    // 逐个样本处理
    for (size_t i = 0; i < n; i++) {
        micInOut[i] = ProcessSample(micInOut[i]);
    }
    return n;
}

bool AcousticEchoCanceller::HasEnoughReference() const {
    std::lock_guard<std::mutex> lock(m_refMutex);
    return m_refWritePos >= m_filterLength;
}

void AcousticEchoCanceller::Reset() {
    std::fill(m_w.begin(), m_w.end(), 0.0f);
    {
        std::lock_guard<std::mutex> lock(m_refMutex);
        std::fill(m_refBuffer.begin(), m_refBuffer.end(), 0.0f);
        m_refWritePos = 0;
    }
    m_samplesProcessed = 0;
    m_runningAvgError = 1.0f;
    m_runningAvgEcho = 1.0f;
    m_erleDb = 0.0f;
    m_converged = false;
}

// ============================================================
// NLMS 核心算法
// ============================================================

float AcousticEchoCanceller::ProcessSample(float micSample) {
    // ---- 步骤1: 从参考缓冲区提取参考帧 ----
    // 参考帧 x[n] 包含最近 filterLength 个参考样本：
    // x[n] = [ref[writePos-1], ref[writePos-2], ..., ref[writePos-filterLength]]

    std::vector<float> refFrame(m_filterLength);

    {
        std::lock_guard<std::mutex> lock(m_refMutex);

        // 如果参考数据不够，直接通过（不作回声消除）
        // 这种情况发生在 TTS 刚开始播放、AEC 还没收到足够参考数据时
        if (m_refWritePos < m_filterLength) {
            m_samplesProcessed++;
            return micSample;
        }

        // 从环形缓冲区读取最近的 filterLength 个参考样本
        // 注意环形缓冲区的取模处理
        for (size_t j = 0; j < m_filterLength; j++) {
            // 当前位置 = 最新写入位置 - 1 - j（逆序：最新在前）
            int idx = static_cast<int>(m_refWritePos) - 1 - static_cast<int>(j);
            if (idx < 0) {
                idx += static_cast<int>(m_refCapacity);
            }
            refFrame[j] = m_refBuffer[static_cast<size_t>(idx)];
        }
    }

    // ---- 步骤2: 计算滤波器输出（估计的回声）----
    // y[n] = w^T · x[n] = Σ w[j] * x[n-j]
    float y = 0.0f;
    float norm = 0.0f;  // ||x[n]||² 参考信号能量
    for (size_t j = 0; j < m_filterLength; j++) {
        y += m_w[j] * refFrame[j];
        norm += refFrame[j] * refFrame[j];
    }

    // ---- 步骤3: 计算误差信号（消除回声后的人声）----
    // e[n] = mic[n] - y[n]
    float e = micSample - y;

    // ---- 步骤4: NLMS 更新滤波器权重 ----
    // w[n+1] = w[n] + μ * e[n] * x[n] / (||x[n]||² + ε)
    if (norm > m_regularization) {
        float mu = m_stepSize / (norm + m_regularization);
        for (size_t j = 0; j < m_filterLength; j++) {
            m_w[j] += mu * e * refFrame[j];
        }
    }

    // ---- 统计与收敛检测 ----
    m_samplesProcessed++;

    // 指数移动平均系数：前1000个样本快速更新，之后慢速
    float alpha = (m_samplesProcessed < 1000) ? 0.01f : 0.001f;

    m_runningAvgEcho   = (1.0f - alpha) * m_runningAvgEcho   + alpha * (y * y);
    m_runningAvgError  = (1.0f - alpha) * m_runningAvgError  + alpha * (e * e);

    // 计算 ERLE (Echo Return Loss Enhancement)
    // ERLE = 10 * log10(E[回声能量] / E[残差能量])
    // ERLE 越高，说明回声消除效果越好
    if (m_runningAvgEcho > 1.0e-10f && m_runningAvgError > 1.0e-10f) {
        float ratio = m_runningAvgEcho / m_runningAvgError;
        m_erleDb = 10.0f * std::log10(ratio + 1.0e-10f);
    }

    // 收敛判定：处理了 5000+ 样本 且 ERLE > 5dB
    if (m_samplesProcessed > 5000 && m_erleDb > 5.0f) {
        m_converged = true;
    }

    return e;  // 返回消除回声后的信号
}
