#include "SherpaTtsEngine.h"
#include "Logger.h"
#include "WavWriter.h"
#include <cstring>
#include <cmath>

// sherpa-onnx C API
#include "sherpa-onnx/c-api.h"

// ============================================================
// SherpaTtsEngine — 基于 sherpa-onnx VITS 的 TTS 引擎
// 从 napi_init.cpp 提取的独立实现
// ============================================================

SherpaTtsEngine::SherpaTtsEngine() {
}

SherpaTtsEngine::~SherpaTtsEngine() {
    Stop();
}

bool SherpaTtsEngine::Init(const std::string& modelDir) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return true;

    m_modelDir = modelDir;

    // 只检查文件存在，不预加载引擎 (在合成时按需创建)
    std::string modelPath  = modelDir + "/model.onnx";
    std::string tokensPath = modelDir + "/tokens.txt";

    if (!FileExists(modelPath)) {
        LOGE("TTS model not found: %s", modelPath.c_str());
        return false;
    }
    if (!FileExists(tokensPath)) {
        LOGE("TTS tokens not found: %s", tokensPath.c_str());
        return false;
    }

    m_initialized = true;
    LOGI("SherpaTtsEngine initialized OK: %s", modelDir.c_str());
    return true;
}

bool SherpaTtsEngine::Synthesize(const std::string& text, float speed) {
    if (!m_initialized) {
        LOGE("TTS not initialized");
        return false;
    }

    m_busy.store(true);
    m_stopping.store(false);

    // 后台线程执行合成
    m_synthThread = std::thread(&SherpaTtsEngine::SynthThreadProc, this, text, speed);
    m_synthThread.detach();

    return true;
}

void SherpaTtsEngine::Stop() {
    m_stopping.store(true);
    m_busy.store(false);
}

bool SherpaTtsEngine::IsBusy() const {
    return m_busy.load();
}

// ============================================================
// 合成线程
// ============================================================

void SherpaTtsEngine::SynthThreadProc(const std::string& text, float speed) {
    LOGI("TTS synthesis starting: text='%s', speed=%.1f", text.c_str(), speed);

    std::string modelPath  = m_modelDir + "/model.onnx";
    std::string tokensPath = m_modelDir + "/tokens.txt";
    std::string lexiconPath = m_modelDir + "/lexicon.txt";
    bool hasLexicon = FileExists(lexiconPath);

    // 配置 sherpa-onnx TTS
    SherpaOnnxOfflineTtsVitsModelConfig vitsConfig;
    memset(&vitsConfig, 0, sizeof(vitsConfig));
    vitsConfig.model = modelPath.c_str();
    vitsConfig.tokens = tokensPath.c_str();
    vitsConfig.lexicon = hasLexicon ? lexiconPath.c_str() : "";
    vitsConfig.noise_scale = 0.667f;
    vitsConfig.noise_scale_w = 0.8f;
    vitsConfig.length_scale = 1.0f / (speed > 0.1f ? speed : 1.0f);

    SherpaOnnxOfflineTtsModelConfig modelConfig;
    memset(&modelConfig, 0, sizeof(modelConfig));
    modelConfig.vits = vitsConfig;
    modelConfig.num_threads = 2;
    modelConfig.debug = 1;
    modelConfig.provider = "cpu";

    SherpaOnnxOfflineTtsConfig ttsConfig;
    memset(&ttsConfig, 0, sizeof(ttsConfig));
    ttsConfig.model = modelConfig;
    ttsConfig.max_num_sentences = 1;

    const SherpaOnnxOfflineTts* tts = SherpaOnnxCreateOfflineTts(&ttsConfig);
    if (!tts) {
        LOGE("SherpaOnnxCreateOfflineTts FAILED");
        m_busy.store(false);
        return;
    }

    int32_t sampleRate = SherpaOnnxOfflineTtsSampleRate(tts);

    // 合成
    const SherpaOnnxGeneratedAudio* audio =
        SherpaOnnxOfflineTtsGenerate(tts, text.c_str(), 0, 1.0);

    if (audio && audio->samples && audio->n > 0) {
        int32_t actualSampleRate = audio->sample_rate > 0 ? audio->sample_rate : sampleRate;
        LOGI("TTS generated %d samples @%dHz", audio->n, actualSampleRate);

        // 调试: 保存 WAV
        WavWriter::WriteWav(m_modelDir + "/../tts_debug.wav",
                            audio->samples, audio->n, actualSampleRate);

        // 回调
        if (OnAudio) {
            OnAudio(audio->samples, audio->n, actualSampleRate, true);
        }
        if (OnProgress) OnProgress(1.0f);
    } else {
        LOGE("TTS generate returned no audio");
    }

    if (audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    SherpaOnnxDestroyOfflineTts(tts);

    m_busy.store(false);
    LOGI("TTS synthesis complete");
}

bool SherpaTtsEngine::FileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
