#define NOMINMAX
#include "WasapiRender.h"
#include "Logger.h"
#include <algorithm>

// ============================================================
// WasapiRender — WASAPI 音频播放
// ============================================================

WasapiRender::WasapiRender() {
}

WasapiRender::~WasapiRender() {
    Stop();
}

bool WasapiRender::Init(int sampleRate) {
    if (m_initialized) return true;
    m_sampleRate = sampleRate;

    HRESULT hr;

    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LOGE("CoInitializeEx failed: 0x%08X", hr);
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator));
    if (FAILED(hr)) { LOGE("MMDeviceEnumerator failed: 0x%08X", hr); return false; }

    hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
    if (FAILED(hr)) { LOGE("GetDefaultAudioEndpoint failed: 0x%08X", hr); return false; }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            (void**)&m_audioClient);
    if (FAILED(hr)) { LOGE("Activate IAudioClient failed: 0x%08X", hr); return false; }

    // 使用设备原生格式 (用 mix format 作为基础)
    WAVEFORMATEX* mixFormat = nullptr;
    hr = m_audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr)) { LOGE("GetMixFormat failed: 0x%08X", hr); return false; }

    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        10000000, 0, mixFormat, nullptr);
    if (FAILED(hr)) {
        LOGE("AudioClient Initialize failed: 0x%08X (trying PCM format)", hr);
        // 回退: 尝试 IEEE_FLOAT mono
        WAVEFORMATEX fmt;
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = 1;
        fmt.nSamplesPerSec = mixFormat->nSamplesPerSec;
        fmt.wBitsPerSample = 32;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = mixFormat->nSamplesPerSec * 4;
        fmt.cbSize = 0;
        hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, &fmt, nullptr);
        if (FAILED(hr)) {
            LOGE("AudioClient Initialize failed: 0x%08X", hr);
            CoTaskMemFree(mixFormat);
            return false;
        }
    }

    // 记录实际格式
    LOGI("Render format: %dHz %dbit %dch",
         mixFormat->nSamplesPerSec, mixFormat->wBitsPerSample, mixFormat->nChannels);
    m_actualSampleRate = mixFormat->nSamplesPerSec;
    CoTaskMemFree(mixFormat);

    hr = m_audioClient->GetService(IID_PPV_ARGS(&m_renderClient));
    if (FAILED(hr)) { LOGE("GetService IAudioRenderClient failed: 0x%08X", hr); return false; }

    m_audioClient->GetBufferSize(&m_bufferSize);
    LOGI("WasapiRender initialized: %dHz, float32 mono, buffer=%u frames",
         m_sampleRate, m_bufferSize);

    m_initialized = true;
    return true;
}

bool WasapiRender::Start() {
    if (!m_initialized || m_isPlaying.load()) return false;

    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr)) { LOGE("AudioClient Start failed: 0x%08X", hr); return false; }

    m_isPlaying.store(true);
    m_renderThread = std::thread(&WasapiRender::RenderLoop, this);
    LOGI("WasapiRender started");
    return true;
}

void WasapiRender::Stop() {
    m_isPlaying.store(false);
    m_queueCv.notify_all();

    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }

    if (m_audioClient) {
        m_audioClient->Stop();
        m_audioClient->Reset();
    }

    // 清空队列
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::queue<float>().swap(m_audioQueue);
    }

    LOGI("WasapiRender stopped");
}

void WasapiRender::EnqueueAudio(const float* samples, int numSamples, int sampleRate) {
    std::lock_guard<std::mutex> lock(m_queueMutex);

    if (sampleRate == m_actualSampleRate || m_actualSampleRate == 0) {
        // 采样率匹配，直接入队
        for (int i = 0; i < numSamples; i++) {
            m_audioQueue.push(samples[i]);
        }
    } else {
        // 采样率不匹配，线性重采样
        float ratio = (float)sampleRate / (float)m_actualSampleRate;
        int outputSamples = (int)(numSamples / ratio);
        for (int i = 0; i < outputSamples; i++) {
            float srcPos = (float)i * ratio;
            int srcIdx = (int)srcPos;
            float frac = srcPos - srcIdx;
            float s0 = samples[std::min(srcIdx, numSamples - 1)];
            float s1 = samples[std::min(srcIdx + 1, numSamples - 1)];
            m_audioQueue.push(s0 + frac * (s1 - s0));
        }
    }
    m_queueCv.notify_one();
}

void WasapiRender::Flush() {
    m_flushing.store(true);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::queue<float>().swap(m_audioQueue);
    }
    if (m_audioClient) {
        m_audioClient->Reset();
    }
    m_flushing.store(false);
    LOGI("WasapiRender flushed");
}

void WasapiRender::RenderLoop() {
    LOGI("Render loop started");

    while (m_isPlaying.load()) {
        // 等待数据
        std::unique_lock<std::mutex> lock(m_queueMutex);
        if (m_audioQueue.empty()) {
            m_queueCv.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }

        // 获取可写入的帧数
        UINT32 padding = 0;
        HRESULT hr = m_audioClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        UINT32 framesAvailable = m_bufferSize - padding;
        if (framesAvailable == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // 从队列取出数据
        UINT32 framesToWrite = (std::min)(framesAvailable, (UINT32)m_audioQueue.size());
        if (framesToWrite == 0) continue;

        std::vector<float> buffer(framesToWrite);
        for (UINT32 i = 0; i < framesToWrite; i++) {
            buffer[i] = m_audioQueue.front();
            m_audioQueue.pop();
        }
        lock.unlock();

        // 写入 WASAPI 缓冲区
        BYTE* renderData = nullptr;
        hr = m_renderClient->GetBuffer(framesToWrite, &renderData);
        if (SUCCEEDED(hr) && renderData) {
            memcpy(renderData, buffer.data(), framesToWrite * sizeof(float));
            m_renderClient->ReleaseBuffer(framesToWrite, 0);
        }
    }

    LOGI("Render loop ended");
}
