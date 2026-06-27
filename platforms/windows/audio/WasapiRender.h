#ifndef WASAPI_RENDER_H
#define WASAPI_RENDER_H

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>

#pragma comment(lib, "ole32.lib")

/**
 * @brief WASAPI 音频播放
 * 
 * 44.1kHz, float32, 单声道, 共享模式。
 * 接收 TTS 引擎的音频数据并播放。
 * 支持 Flush() 用于打断功能。
 */
class WasapiRender {
public:
    WasapiRender();
    ~WasapiRender();

    bool Init(int sampleRate = 44100);
    bool Start();
    void Stop();
    bool IsPlaying() const { return m_isPlaying.load(); }

    /// 入队音频数据
    void EnqueueAudio(const float* samples, int numSamples, int sampleRate);

    /// 清空所有缓冲 (打断用)
    void Flush();

private:
    void RenderLoop();

    IMMDeviceEnumerator* m_enumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioClient* m_audioClient = nullptr;
    IAudioRenderClient* m_renderClient = nullptr;

    std::thread m_renderThread;
    std::atomic<bool> m_isPlaying{false};
    std::atomic<bool> m_initialized{false};
    int m_actualSampleRate = 48000;
    std::atomic<bool> m_flushing{false};

    int m_sampleRate = 44100;
    UINT32 m_bufferSize = 0;

    // 内部音频队列
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::queue<float> m_audioQueue;
    std::atomic<bool> m_queueEos{false};
};

#endif // WASAPI_RENDER_H
