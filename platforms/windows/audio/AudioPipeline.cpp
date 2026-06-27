#include "AudioPipeline.h"
#include "../../../src/utils/Logger.h"

// ============================================================
// AudioPipeline — Windows 全双工音频管线
// ============================================================

AudioPipeline::AudioPipeline() {
}

AudioPipeline::~AudioPipeline() {
    StopAll();
}

bool AudioPipeline::Init(int captureRate, int renderRate) {
    bool ok = true;
    ok = m_capture.Init(captureRate) && ok;
    ok = m_render.Init(renderRate) && ok;

    // 注册采集回调
    m_capture.OnAudioData = [this](const int16_t* pcm, int numSamples) {
        this->OnCaptureData(pcm, numSamples);
    };

    LOGI("AudioPipeline initialized");
    return ok;
}

bool AudioPipeline::StartCapture() {
    return m_capture.Start();
}

bool AudioPipeline::StartPlayback() {
    return m_render.Start();
}

void AudioPipeline::StopAll() {
    m_capture.Stop();
    m_render.Stop();
    LOGI("AudioPipeline stopped");
}

void AudioPipeline::PlayTtsAudio(const float* samples, int numSamples, int sampleRate) {
    m_render.EnqueueAudio(samples, numSamples, sampleRate);
}

void AudioPipeline::InterruptPlayback() {
    m_render.Flush();
    LOGI("AudioPipeline playback interrupted");
}

void AudioPipeline::BindAsrEngine(AsrEngine* engine) {
    m_asrEngine = engine;
}

void AudioPipeline::OnCaptureData(const int16_t* pcm, int numSamples) {
    if (!m_asrEngine || !m_asrEngine->IsRunning()) return;

    // int16 → float32 并喂入 ASR
    // (ASR 引擎要求 16kHz float32 PCM)
    std::vector<float> floatSamples(numSamples);
    for (int i = 0; i < numSamples; i++) {
        floatSamples[i] = (float)pcm[i] / 32768.0f;
    }
    m_asrEngine->FeedAudio(floatSamples.data(), numSamples);
}
