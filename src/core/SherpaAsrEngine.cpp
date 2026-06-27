#include "SherpaAsrEngine.h"
#include "Logger.h"
#include <cstring>
#include <algorithm>
#include <cmath>

// sherpa-onnx C API
#include "sherpa-onnx/c-api.h"

// ============================================================
// SherpaAsrEngine — 基于 sherpa-onnx 的 ASR 引擎
// 从 napi_init.cpp 提取的独立实现
// ============================================================

SherpaAsrEngine::SherpaAsrEngine() {
    m_lastPerfPush = std::chrono::steady_clock::now();
}

SherpaAsrEngine::~SherpaAsrEngine() {
    Stop();
}

bool SherpaAsrEngine::Init(const std::string& modelDir,
                            const std::string& vadModel,
                            const std::string& kwsModel) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return true;

    LOGI("SherpaAsrEngine::Init modelDir=%s vadModel=%s kwsModel=%s",
         modelDir.c_str(), vadModel.c_str(), kwsModel.c_str());

    // 1. 配置模型路径
    std::string encoderPath = modelDir + "/encoder-epoch-99-avg-1.int8.onnx";
    std::string decoderPath = modelDir + "/decoder-epoch-99-avg-1.int8.onnx";
    std::string joinerPath  = modelDir + "/joiner-epoch-99-avg-1.onnx";
    std::string tokensPath  = modelDir + "/tokens.txt";

    // 2. 检查文件
    auto checkFile = [](const std::string& path) -> bool {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fclose(f);
        return true;
    };

    if (!checkFile(encoderPath)) { LOGE("Missing encoder: %s", encoderPath.c_str()); return false; }
    if (!checkFile(decoderPath)) { LOGE("Missing decoder: %s", decoderPath.c_str()); return false; }
    if (!checkFile(joinerPath))  { LOGE("Missing joiner: %s",  joinerPath.c_str());  return false; }
    if (!checkFile(tokensPath))  { LOGE("Missing tokens: %s",  tokensPath.c_str());  return false; }

    // 3. 创建 recognizer
    SherpaOnnxOnlineTransducerModelConfig transducer;
    memset(&transducer, 0, sizeof(transducer));
    transducer.encoder = encoderPath.c_str();
    transducer.decoder = decoderPath.c_str();
    transducer.joiner  = joinerPath.c_str();

    SherpaOnnxOnlineModelConfig modelConfig;
    memset(&modelConfig, 0, sizeof(modelConfig));
    modelConfig.transducer = transducer;
    modelConfig.tokens = tokensPath.c_str();
    modelConfig.num_threads = 2;
    modelConfig.debug = 1;
    modelConfig.provider = "cpu";

    SherpaOnnxFeatureConfig featConfig;
    memset(&featConfig, 0, sizeof(featConfig));
    featConfig.sample_rate = 16000;
    featConfig.feature_dim = 80;

    SherpaOnnxOnlineRecognizerConfig recognizerConfig;
    memset(&recognizerConfig, 0, sizeof(recognizerConfig));
    recognizerConfig.feat_config = featConfig;
    recognizerConfig.model_config = modelConfig;
    recognizerConfig.decoding_method = "greedy_search";
    recognizerConfig.max_active_paths = 4;
    recognizerConfig.enable_endpoint = 1;
    recognizerConfig.rule1_min_trailing_silence = 2.4f;
    recognizerConfig.rule2_min_trailing_silence = 1.2f;
    recognizerConfig.rule3_min_utterance_length = 20.0f;

    m_recognizer = const_cast<SherpaOnnxOnlineRecognizer*>(
        SherpaOnnxCreateOnlineRecognizer(&recognizerConfig));
    if (!m_recognizer) {
        LOGE("SherpaOnnxCreateOnlineRecognizer FAILED");
        return false;
    }

    m_stream = const_cast<SherpaOnnxOnlineStream*>(
        SherpaOnnxCreateOnlineStream(m_recognizer));
    if (!m_stream) {
        LOGE("SherpaOnnxCreateOnlineStream FAILED");
        SherpaOnnxDestroyOnlineRecognizer(m_recognizer);
        m_recognizer = nullptr;
        return false;
    }

    // 4. 创建 VAD (可选)
    if (!vadModel.empty()) {
        FILE* fVad = fopen(vadModel.c_str(), "rb");
        if (fVad) {
            fclose(fVad);
            SherpaOnnxSileroVadModelConfig sileroConfig;
            memset(&sileroConfig, 0, sizeof(sileroConfig));
            sileroConfig.model = vadModel.c_str();
            sileroConfig.threshold = 0.5f;
            sileroConfig.min_silence_duration = 0.5f;
            sileroConfig.min_speech_duration = 0.2f;
            sileroConfig.window_size = 256;
            sileroConfig.max_speech_duration = 30.0f;

            SherpaOnnxVadModelConfig vadConfig;
            memset(&vadConfig, 0, sizeof(vadConfig));
            vadConfig.silero_vad = sileroConfig;
            vadConfig.sample_rate = 16000;
            vadConfig.num_threads = 1;
            vadConfig.provider = "cpu";

            m_vad = const_cast<SherpaOnnxVoiceActivityDetector*>(
                SherpaOnnxCreateVoiceActivityDetector(&vadConfig, 30.0f));
            LOGI("VAD created: %p", m_vad);
        }
    }

    // 5. 创建 Keyword Spotter (可选)
    if (!kwsModel.empty()) {
        // 简化: 暂不实现完整的 keyword spotter 初始化
        LOGI("KWS model provided but not initialized: %s", kwsModel.c_str());
    }

    m_initialized = true;
    LOGI("SherpaAsrEngine initialized OK");
    return true;
}

bool SherpaAsrEngine::Start() {
    if (!m_initialized || m_running.load()) return false;

    m_running.store(true);
    m_wakeWordDetected.store(true);  // 暂时跳过唤醒词
    m_ringBuffer.Clear();
    m_rtfMetrics.Reset();
    m_lastText.clear();
    m_lastPerfPush = std::chrono::steady_clock::now();

    m_decodeThread = std::thread(&SherpaAsrEngine::DecodeLoop, this);
    LOGI("SherpaAsrEngine started");
    return true;
}

void SherpaAsrEngine::FeedAudio(const float* samples, int numSamples) {
    if (!m_running.load()) return;

    // 写入环形缓冲区
    size_t written = m_ringBuffer.Write(samples, static_cast<size_t>(numSamples));
    if (written < static_cast<size_t>(numSamples)) {
        LOGW("Ring buffer full: wrote %zu/%d", written, numSamples);
    }
}

void SherpaAsrEngine::Stop() {
    m_running.store(false);

    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }

    LOGI("SherpaAsrEngine stopped");
}

bool SherpaAsrEngine::IsRunning() const {
    return m_running.load();
}

void SherpaAsrEngine::Reset() {
    Stop();
    m_ringBuffer.Clear();
    m_rtfMetrics.Reset();
    m_lastText.clear();
    LOGI("SherpaAsrEngine reset");
}

// ============================================================
// 解码线程
// ============================================================

void SherpaAsrEngine::DecodeLoop() {
    LOGI("DecodeLoop started");
    constexpr size_t kReadChunkSize = 5120;  // 320ms @16kHz
    std::vector<float> vadAccumulator;

    while (m_running.load()) {
        // 从环形缓冲区读取
        std::vector<float> localBuf;
        localBuf.reserve(kReadChunkSize);
        size_t samplesRead = m_ringBuffer.ReadToVector(localBuf, kReadChunkSize);

        if (samplesRead == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto decodeStart = std::chrono::steady_clock::now();

        // 喂入 ASR
        SherpaOnnxOnlineStreamAcceptWaveform(m_stream, 16000,
                                             localBuf.data(), (int32_t)localBuf.size());

        while (SherpaOnnxIsOnlineStreamReady(m_recognizer, m_stream)) {
            SherpaOnnxDecodeOnlineStream(m_recognizer, m_stream);
        }

        auto decodeEnd = std::chrono::steady_clock::now();
        auto decodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            decodeEnd - decodeStart).count();

        // 计算 RTF
        double audioDurationMs = (double)samplesRead / 16.0;
        double rtf = audioDurationMs > 0 ? (double)decodeMs / audioDurationMs : 0;
        m_rtfMetrics.RecordRtf(rtf);
        m_rtfMetrics.totalDecodeMs.fetch_add(decodeMs);
        m_rtfMetrics.totalAudioMs.fetch_add((int64_t)audioDurationMs);
        m_rtfMetrics.lastLatencyMs.store(decodeMs);

        // 获取结果
        const auto* r = SherpaOnnxGetOnlineStreamResult(m_recognizer, m_stream);
        if (r && r->text && strlen(r->text) > 0) {
            std::string currentText(r->text);
            if (currentText != m_lastText) {
                m_lastText = currentText;
                LOGI("ASR partial: %s (RTF=%.3f)", currentText.c_str(), rtf);
                if (OnResult) OnResult(currentText, false);
            }
        }
        if (r) SherpaOnnxDestroyOnlineRecognizerResult(r);

        // 端点检测
        if (SherpaOnnxOnlineStreamIsEndpoint(m_recognizer, m_stream)) {
            if (!m_lastText.empty()) {
                LOGI("ASR endpoint: %s", m_lastText.c_str());
                if (OnResult) OnResult(m_lastText, true);
                m_lastText.clear();
                m_ringBuffer.Clear();
            }
            SherpaOnnxOnlineStreamReset(m_recognizer, m_stream);
        }

        // 推送性能指标 (每500ms)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastPerfPush).count();
        if (elapsed >= 500) {
            m_lastPerfPush = now;
            if (OnPerfMetrics) {
                OnPerfMetrics(
                    m_rtfMetrics.currentRtf.load(),
                    m_rtfMetrics.avgRtf.load(),
                    m_rtfMetrics.maxRtf.load(),
                    m_rtfMetrics.lastLatencyMs.load(),
                    m_rtfMetrics.vadLatencyMs.load(),
                    m_rtfMetrics.needsDegradation.load()
                );
            }
        }
    }

    LOGI("DecodeLoop finished");
}

// ============================================================
// RTF 监控
// ============================================================

void SherpaAsrEngine::RtfMetrics::RecordRtf(double rtf) {
    currentRtf.store(rtf);
    std::lock_guard<std::mutex> lock(windowMutex);
    rtfWindow.push_back(rtf);
    if (rtfWindow.size() > 50) rtfWindow.pop_front();

    double sum = 0;
    for (auto v : rtfWindow) sum += v;
    avgRtf.store(sum / rtfWindow.size());

    if (rtf > maxRtf.load()) maxRtf.store(rtf);

    // 连续5帧 RTF > 0.3 触发降级
    if (rtfWindow.size() >= 5) {
        int count = 0;
        for (auto it = rtfWindow.rbegin(); it != rtfWindow.rend() && count < 5; ++it, ++count) {
            if (*it > 0.3) {
                needsDegradation.store(true);
                LOGW("RTF=%.3f > 0.3, degradation triggered", *it);
                break;
            }
        }
    }
}

void SherpaAsrEngine::RtfMetrics::Reset() {
    currentRtf.store(0.0);
    avgRtf.store(0.0);
    maxRtf.store(0.0);
    totalAudioMs.store(0);
    totalDecodeMs.store(0);
    lastLatencyMs.store(0);
    needsDegradation.store(false);
    std::lock_guard<std::mutex> lock(windowMutex);
    rtfWindow.clear();
}
