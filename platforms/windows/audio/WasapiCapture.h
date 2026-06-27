#ifndef WASAPI_CAPTURE_H
#define WASAPI_CAPTURE_H

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

/**
 * @brief WASAPI 麦克风采集
 * 
 * 16kHz, 16-bit, 单声道, 共享模式。
 * 采集的 PCM 数据通过回调传递给 ASR 引擎。
 */
class WasapiCapture {
public:
    WasapiCapture();
    ~WasapiCapture();

    bool Init(int sampleRate = 16000);
    bool Start();
    void Stop();
    bool IsCapturing() const { return m_isCapturing.load(); }

    /// 音频数据回调 (int16 PCM)
    std::function<void(const int16_t* pcm, int numSamples)> OnAudioData;

private:
    void CaptureLoop();

    IMMDeviceEnumerator* m_enumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    WAVEFORMATEX* m_mixFormat = nullptr;

    std::thread m_captureThread;
    std::atomic<bool> m_isCapturing{false};
    std::atomic<bool> m_initialized{false};

    int m_sampleRate = 16000;
    int m_channels = 1;
    int m_bytesPerFrame = 2;  // 16-bit mono

    // 实际设备格式 (用于重采样)
    int m_actualSampleRate = 48000;
    int m_actualChannels = 2;
    int m_actualBitsPerSample = 32;
    int m_actualBytesPerFrame = 8;
    // 重采样输出缓冲区 (int16)
    std::vector<int16_t> m_resampleBuf;
};

#endif // WASAPI_CAPTURE_H
