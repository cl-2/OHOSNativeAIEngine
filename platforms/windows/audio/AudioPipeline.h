#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

#include "WasapiCapture.h"
#include "WasapiRender.h"
#include "../../../src/core/RingBuffer.h"
#include "../../../src/core/AsrEngine.h"
#include <atomic>
#include <thread>

/**
 * @brief Windows 音频管线
 * 
 * 串联 WASAPI 麦克风采集 → RingBuffer → ASR 引擎
 * 以及 TTS 引擎 → WASAPI 扬声器播放
 * 支持打断检测
 */
class AudioPipeline {
public:
    AudioPipeline();
    ~AudioPipeline();

    bool Init(int captureRate = 16000, int renderRate = 44100);
    bool StartCapture();
    bool StartPlayback();
    void StopAll();
    bool IsCapturing() const { return m_capture.IsCapturing(); }
    bool IsPlaying() const { return m_render.IsPlaying(); }

    /// 将 TTS 音频送入播放
    void PlayTtsAudio(const float* samples, int numSamples, int sampleRate);

    /// 立即停止播放 (打断)
    void InterruptPlayback();

    /// 绑定 ASR 引擎 (音频采集数据自动喂入)
    void BindAsrEngine(AsrEngine* engine);

private:
    WasapiCapture m_capture;
    WasapiRender  m_render;
    AsrEngine*    m_asrEngine = nullptr;
    std::atomic<bool> m_running{false};

    // 采集 → ASR 回调
    void OnCaptureData(const int16_t* pcm, int numSamples);
};

#endif // AUDIO_PIPELINE_H
