#include "Aec.h"

#include <algorithm>
#include <cmath>

AcousticEchoCanceller::AcousticEchoCanceller(size_t filterLength, float stepSize)
    : m_filterLength(filterLength)
    , m_stepSize(stepSize)
    , m_regularization(1.0e-6f)
    , m_w(filterLength, 0.0f)
    , m_refCapacity(filterLength * 32)
    , m_refFifo(filterLength * 32, 0.0f)
    , m_refHistory(filterLength, 0.0f)
    , m_refWritePos(0)
    , m_refReadPos(0)
    , m_refAvailable(0)
    , m_refHistoryPos(0)
    , m_refHistoryCount(0)
    , m_samplesProcessed(0)
    , m_runningAvgError(1.0f)
    , m_runningAvgEcho(1.0f)
    , m_erleDb(0.0f)
    , m_active(true) {
}

void AcousticEchoCanceller::AddTtsReference(const float* samples, size_t n) {
    if (!samples || n == 0) return;

    std::lock_guard<std::mutex> lock(m_refMutex);
    for (size_t i = 0; i < n; ++i) {
        if (m_refAvailable == m_refCapacity) {
            m_refReadPos = (m_refReadPos + 1) % m_refCapacity;
            --m_refAvailable;
        }
        m_refFifo[m_refWritePos] = samples[i];
        m_refWritePos = (m_refWritePos + 1) % m_refCapacity;
        ++m_refAvailable;
    }
}

size_t AcousticEchoCanceller::ProcessMicAudio(float* micInOut, size_t n) {
    if (!m_active || !micInOut || n == 0) return n;
    for (size_t i = 0; i < n; ++i) micInOut[i] = ProcessSample(micInOut[i]);
    return n;
}

bool AcousticEchoCanceller::HasEnoughReference() const {
    std::lock_guard<std::mutex> lock(m_refMutex);
    return m_refHistoryCount >= m_filterLength && m_refAvailable > 0;
}

void AcousticEchoCanceller::Reset() {
    std::fill(m_w.begin(), m_w.end(), 0.0f);
    {
        std::lock_guard<std::mutex> lock(m_refMutex);
        std::fill(m_refFifo.begin(), m_refFifo.end(), 0.0f);
        std::fill(m_refHistory.begin(), m_refHistory.end(), 0.0f);
        m_refWritePos = 0;
        m_refReadPos = 0;
        m_refAvailable = 0;
        m_refHistoryPos = 0;
        m_refHistoryCount = 0;
    }
    m_samplesProcessed = 0;
    m_runningAvgError = 1.0f;
    m_runningAvgEcho = 1.0f;
    m_erleDb = 0.0f;
    m_converged = false;
}

float AcousticEchoCanceller::ProcessSample(float micSample) {
    std::vector<float> refFrame(m_filterLength);
    {
        std::lock_guard<std::mutex> lock(m_refMutex);
        if (m_refAvailable == 0) {
            ++m_samplesProcessed;
            return micSample;
        }

        const float reference = m_refFifo[m_refReadPos];
        m_refReadPos = (m_refReadPos + 1) % m_refCapacity;
        --m_refAvailable;
        m_refHistory[m_refHistoryPos] = reference;
        m_refHistoryPos = (m_refHistoryPos + 1) % m_filterLength;
        m_refHistoryCount = std::min(m_refHistoryCount + 1, m_filterLength);
        if (m_refHistoryCount < m_filterLength) {
            ++m_samplesProcessed;
            return micSample;
        }

        for (size_t j = 0; j < m_filterLength; ++j) {
            int idx = static_cast<int>(m_refHistoryPos) - 1 - static_cast<int>(j);
            if (idx < 0) idx += static_cast<int>(m_filterLength);
            refFrame[j] = m_refHistory[static_cast<size_t>(idx)];
        }
    }

    float echo = 0.0f;
    float norm = 0.0f;
    for (size_t j = 0; j < m_filterLength; ++j) {
        echo += m_w[j] * refFrame[j];
        norm += refFrame[j] * refFrame[j];
    }

    const float error = micSample - echo;
    if (norm > m_regularization) {
        const float mu = m_stepSize / (norm + m_regularization);
        for (size_t j = 0; j < m_filterLength; ++j) m_w[j] += mu * error * refFrame[j];
    }

    ++m_samplesProcessed;
    const float alpha = (m_samplesProcessed < 1000) ? 0.01f : 0.001f;
    m_runningAvgEcho = (1.0f - alpha) * m_runningAvgEcho + alpha * echo * echo;
    m_runningAvgError = (1.0f - alpha) * m_runningAvgError + alpha * error * error;
    if (m_runningAvgEcho > 1.0e-10f && m_runningAvgError > 1.0e-10f) {
        m_erleDb = 10.0f * std::log10(m_runningAvgEcho / m_runningAvgError + 1.0e-10f);
    }
    if (m_samplesProcessed > 5000 && m_erleDb > 5.0f) m_converged = true;
    return error;
}
