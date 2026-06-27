#define NOMINMAX
#include "WasapiCapture.h"
#include "Logger.h"
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <algorithm>

// ============================================================
// WasapiCapture �?WASAPI 麦克风采�?
// ============================================================

WasapiCapture::WasapiCapture() {
}

WasapiCapture::~WasapiCapture() {
    Stop();
    if (m_mixFormat) CoTaskMemFree(m_mixFormat);
    if (m_enumerator) m_enumerator->Release();
}

bool WasapiCapture::Init(int sampleRate) {
    if (m_initialized) return true;
    m_sampleRate = sampleRate;

    HRESULT hr;

    // 1. 初始�?COM
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        LOGE("CoInitializeEx failed: 0x%08X", hr);
        return false;
    }

    // 2. 创建设备枚举�?
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, IID_PPV_ARGS(&m_enumerator));
    if (FAILED(hr)) {
        LOGE("CoCreateInstance MMDeviceEnumerator failed: 0x%08X", hr);
        return false;
    }

    // 3. 获取默认麦克�?
    hr = m_enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &m_device);
    if (FAILED(hr)) {
        LOGE("GetDefaultAudioEndpoint failed: 0x%08X (no microphone?)", hr);
        return false;
    }

    // 4. 激�?IAudioClient
    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            (void**)&m_audioClient);
    if (FAILED(hr)) {
        LOGE("Activate IAudioClient failed: 0x%08X", hr);
        return false;
    }

    // 5. 获取混合格式
    hr = m_audioClient->GetMixFormat(&m_mixFormat);
    if (FAILED(hr)) {
        LOGE("GetMixFormat failed: 0x%08X", hr);
        return false;
    }

    // 6. 使用设备原生格式初始�?(共享模式下兼容性最�?
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_NOPERSIST,
        10000000, 0, m_mixFormat, nullptr);
    if (FAILED(hr)) {
        LOGE("AudioClient Initialize failed: 0x%08X", hr);
        return false;
    }

    // 记录实际格式
    LOGI("Capture format: %dHz %dbit %dch",
         m_mixFormat->nSamplesPerSec, m_mixFormat->wBitsPerSample,
         m_mixFormat->nChannels);
    m_actualSampleRate = m_mixFormat->nSamplesPerSec;
    m_actualChannels = m_mixFormat->nChannels;
    m_actualBitsPerSample = m_mixFormat->wBitsPerSample;
    m_actualBytesPerFrame = m_mixFormat->nBlockAlign;

    // 8. 获取 CaptureClient
    hr = m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient));
    if (FAILED(hr)) {
        LOGE("GetService IAudioCaptureClient failed: 0x%08X", hr);
        return false;
    }

    m_initialized = true;
    LOGI("WasapiCapture initialized: %dHz, 16-bit mono", m_sampleRate);
    return true;
}

bool WasapiCapture::Start() {
    if (!m_initialized || m_isCapturing.load()) return false;

    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr)) {
        LOGE("AudioClient Start failed: 0x%08X", hr);
        return false;
    }

    m_isCapturing.store(true);
    m_captureThread = std::thread(&WasapiCapture::CaptureLoop, this);
    LOGI("WasapiCapture started");
    return true;
}

void WasapiCapture::Stop() {
    m_isCapturing.store(false);

    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }

    if (m_audioClient) {
        m_audioClient->Stop();
    }

    LOGI("WasapiCapture stopped");
}

void WasapiCapture::CaptureLoop() {
    // 获取缓冲区大�?
    UINT32 bufferSize = 0;
    m_audioClient->GetBufferSize(&bufferSize);
    LOGI("Capture buffer size: %u frames", bufferSize);

    // 获取实际周期 (ms)
    REFERENCE_TIME defaultPeriod;
    REFERENCE_TIME minimumPeriod;
    IMMDeviceEnumerator* tempEnum = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                     CLSCTX_ALL, IID_PPV_ARGS(&tempEnum));
    if (tempEnum) {
        IMMDevice* tempDev = nullptr;
        tempEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &tempDev);
        if (tempDev) {
            IAudioClient* tempClient = nullptr;
            tempDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             (void**)&tempClient);
            if (tempClient) {
                tempClient->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
                LOGI("WASAPI period: default=%lldms, min=%lldms",
                     defaultPeriod / 10000, minimumPeriod / 10000);
                tempClient->Release();
            }
            tempDev->Release();
        }
        tempEnum->Release();
    }

    UINT32 packetSize = 0;
    const int sleepMs = 20;  // 20ms 轮询间隔

    while (m_isCapturing.load()) {
        Sleep(sleepMs);

        HRESULT hr = m_captureClient->GetNextPacketSize(&packetSize);
        if (FAILED(hr)) continue;

        while (packetSize > 0) {
            BYTE* data = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            hr = m_captureClient->GetBuffer(&data, &numFramesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr) || !data || numFramesAvailable == 0) break;

            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                // 将设备原生格式重采样�?16kHz 16-bit mono
                int inputSamples = static_cast<int>(numFramesAvailable);
                int outputSamples = inputSamples * m_sampleRate / m_actualSampleRate;

                m_resampleBuf.resize(outputSamples);

                if (m_actualBitsPerSample == 32 && m_actualChannels >= 1) {
                    // 32-bit float �?16kHz int16 mono
                    const float* src = reinterpret_cast<const float*>(data);
                    for (int i = 0; i < outputSamples; i++) {
                        float srcPos = (float)i * m_actualSampleRate / m_sampleRate;
                        int srcIdx = (int)srcPos;
                        float frac = srcPos - srcIdx;
                        float s0, s1;
                        int nextIdx = std::min(srcIdx + 1, inputSamples - 1);
                        if (m_actualChannels >= 2) {
                            s0 = (src[srcIdx * 2] + src[srcIdx * 2 + 1]) * 0.5f;
                            s1 = (src[nextIdx * 2] + src[nextIdx * 2 + 1]) * 0.5f;
                        } else {
                            s0 = src[srcIdx];
                            s1 = src[nextIdx];
                        }
                        float sample = s0 + frac * (s1 - s0);
                        sample = std::max(-1.0f, std::min(1.0f, sample));
                        m_resampleBuf[i] = (int16_t)(sample * 32768.0f);
                    }
                } else {
                    // 其他格式 �?16kHz int16 mono (混音+重采�?
                    const int16_t* src = reinterpret_cast<const int16_t*>(data);
                    for (int i = 0; i < outputSamples; i++) {
                        float srcPos = (float)i * m_actualSampleRate / m_sampleRate;
                        int srcIdx = (int)srcPos;
                        float frac = srcPos - srcIdx;
                        float s0, s1;
                        int nextIdx = std::min(srcIdx + 1, inputSamples - 1);
                        if (m_actualChannels >= 2) {
                            s0 = (src[srcIdx * 2] + src[srcIdx * 2 + 1]) / 65536.0f;
                            s1 = (src[nextIdx * 2] + src[nextIdx * 2 + 1]) / 65536.0f;
                        } else {
                            s0 = src[srcIdx] / 32768.0f;
                            s1 = src[nextIdx] / 32768.0f;
                        }
                        float sample = s0 + frac * (s1 - s0);
                        sample = std::max(-1.0f, std::min(1.0f, sample));
                        m_resampleBuf[i] = (int16_t)(sample * 32768.0f);
                    }
                }

                // 回调 int16 PCM (16kHz mono)
                if (OnAudioData && !m_resampleBuf.empty()) {
                    OnAudioData(m_resampleBuf.data(),
                                static_cast<int>(m_resampleBuf.size()));
                }
            }

            m_captureClient->ReleaseBuffer(numFramesAvailable);
            m_captureClient->GetNextPacketSize(&packetSize);
        }
    }

    LOGI("Capture loop ended");
}
