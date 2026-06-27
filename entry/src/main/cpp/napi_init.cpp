#include "napi/native_api.h"
#include "hilog/log.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "OHOS_Native_ai_engine"
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>
#include <cstring>
#include <iostream>
#include <mutex>
#include <cstdio>
#include <unistd.h>

// 引入新版 sherpa-onnx C API
#include "sherpa-onnx/c-api.h"
#include "RingBuffer.h"
#include <map>

// 无锁环形缓冲区：音频采集（生产者）→ ASR 推理（消费者）
// 容量 320000 float ≈ 20秒 @16kHz，避免 mutex 竞争
static RingBuffer<float> g_audioRingBuffer(320000);

// RawFile API - 从 HAP 读取模型文件
#include "rawfile/raw_file_manager.h"
#include "rawfile/raw_file.h"
static NativeResourceManager* g_nativeResMgr = nullptr;

// 全双工音频状态机
#include "audio/AudioStateMachine.h"
#include "llm/LocalLlmEngine.h"
#include "llm/RemoteLlmEngine.h"
static AudioStateMachine g_audioStateMachine;
static AudioStateMachineConfig g_audioStateMachineConfig;

// 声学回声消除 (AEC) - NLMS 自适应滤波器
// 使用 unique_ptr + 显式初始化，避免静态初始化顺序问题
#include "audio/Aec.h"
#include <memory>
static std::unique_ptr<AcousticEchoCanceller> g_aec;

// 性能监控采集器
#include "metrics/MetricsCollector.h"

// --- 性能监控工具 ---
class PerformanceProfiler {
public:
    static void Start(const std::string& tag) {
        m_startTimes[tag] = std::chrono::steady_clock::now();
    }

    static void End(const std::string& tag) {
        auto it = m_startTimes.find(tag);
        if (it != m_startTimes.end()) {
            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - it->second).count();
            OH_LOG_INFO(LOG_APP, "ohos_Perf [%{public}s] Latency: %{public}lld ms", tag.c_str(), duration);
            m_startTimes.erase(it);
        }
    }

private:
    static std::map<std::string, std::chrono::steady_clock::time_point> m_startTimes;
};

std::map<std::string, std::chrono::steady_clock::time_point> PerformanceProfiler::m_startTimes;

// FeedAudio 内部状态（替代静态局部变量，支持 Reset）
struct FeedAudioState {
    float prevInput = 0.0f;
    float prevOutput = 0.0f;
    std::chrono::steady_clock::time_point ttsEndTime;
    bool prevTtsPlaying = false;
    int aecLogCounter = 0;
    int feedCounter = 0;
};
static FeedAudioState g_feedAudioState;

static void ResetFeedAudioState() {
    g_feedAudioState.prevInput = 0.0f;
    g_feedAudioState.prevOutput = 0.0f;
    g_feedAudioState.ttsEndTime = std::chrono::steady_clock::time_point();
    g_feedAudioState.prevTtsPlaying = false;
    g_feedAudioState.aecLogCounter = 0;
    g_feedAudioState.feedCounter = 0;
    g_audioRingBuffer.Clear();
}

// ============================================================
// ASR + VAD 实现
// ============================================================

// --- ASR 全局状态 ---
static std::mutex g_asrMutex;
static std::atomic<bool> g_isAsrRunning(false);
// 全双工 Action 回调 TSFN（状态机 → ArkTS）
static napi_threadsafe_function g_duplexActionTsFunction = nullptr;

// Action 回调数据结构
struct DuplexActionData {
    std::string action;
    std::string payload;
};

static const SherpaOnnxOnlineRecognizer* g_asrRecognizer = nullptr;
static const SherpaOnnxOnlineStream* g_asrStream = nullptr;
static const SherpaOnnxVoiceActivityDetector* g_vad = nullptr;
static std::atomic<bool> g_asrPrewarmed(false);  // ASR 引擎已预热

// TTS 全局引擎（常驻，避免每次重新加载 170MB 模型）
static const SherpaOnnxOfflineTts* g_ttsEngine = nullptr;
static std::mutex g_ttsMutex;
static std::string g_ttsModelDir;

// --- Wake‑Word (Keyword Spotting) globals ---
static const SherpaOnnxKeywordSpotter* g_keywordSpotter = nullptr;
static const SherpaOnnxOnlineStream* g_keywordStream = nullptr;
static std::atomic<bool> g_wakeWordDetected(false);


// --- 音频缓冲区（JS 线程写入，ASR 线程读取）---
static std::mutex g_audioBufMutex;
static std::vector<float> g_audioBuffer;

// ASR 工作线程
static std::thread g_asrThread;

// --- Action JS 回调（状态机 → ArkTS）---
static void DuplexActionCallJsCallback(napi_env env, napi_value jsCallback, void* context, void* data) {
    if (!data) return;
    DuplexActionData* cbData = static_cast<DuplexActionData*>(data);
    napi_value undefined;
    napi_get_undefined(env, &undefined);

    napi_value jsAction;
    napi_create_string_utf8(env, cbData->action.c_str(), NAPI_AUTO_LENGTH, &jsAction);
    napi_value jsPayload;
    napi_create_string_utf8(env, cbData->payload.c_str(), NAPI_AUTO_LENGTH, &jsPayload);

    napi_value argv[2] = { jsAction, jsPayload };
    napi_call_function(env, undefined, jsCallback, 2, argv, nullptr);
    delete cbData;
}

// --- ASR 后台解码线程 ---
static void BackgroundAsrThread() {
    OH_LOG_INFO(LOG_APP, "ohos_ASR BackgroundAsrThread started");
    std::string lastText = "";

    // 如果没有 keyword spotter，跳过唤醒词检测，直接开始 ASR
    if (!g_keywordSpotter) {
        g_wakeWordDetected.store(true);
        OH_LOG_INFO(LOG_APP, "ohos_ASR no keyword spotter, auto-skip wake-word detection");
    }

    while (g_isAsrRunning.load()) {
        // 从环形缓冲区取音频数据（无锁、零拷贝）
        std::vector<float> localBuf;
        localBuf.reserve(5120);  // 每次读取 ~320ms 音频 (16kHz × 0.32s)
        size_t samplesRead = g_audioRingBuffer.ReadToVector(localBuf, 5120);

        // 每 100 次循环打印一次 RingBuffer 状态
        static int logCounter = 0;
        if (++logCounter % 100 == 0) {
            OH_LOG_INFO(LOG_APP, "ohos_ASR BGThread alive: ringBuf.size=%{public}zu, samplesRead=%{public}zu, isRunning=%{public}d",
                g_audioRingBuffer.Size(), samplesRead, g_isAsrRunning.load());
        }

        if (samplesRead == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // 定期检查 VAD/ASR 状态
        {
            static int debugCounter = 0;
            if (++debugCounter % 50 == 0) {
                OH_LOG_INFO(LOG_APP, "ohos_ASR_STATE: ring=%{public}zu vad=%{public}d stream=%{public}d wake=%{public}d running=%{public}d",
                    g_audioRingBuffer.Size(), g_vad ? !SherpaOnnxVoiceActivityDetectorEmpty(g_vad) : -1,
                    g_asrStream != nullptr, g_wakeWordDetected.load(), g_isAsrRunning.load());
            }
        }

        // --- Wake‑Word detection before ASR processing ---
        if (!g_wakeWordDetected.load() && g_keywordSpotter && g_keywordStream) {
            SherpaOnnxOnlineStreamAcceptWaveform(g_keywordStream, 16000, localBuf.data(), (int32_t)localBuf.size());
            PerformanceProfiler::Start("Wake-Word Detection");
            while (SherpaOnnxIsKeywordStreamReady(g_keywordSpotter, g_keywordStream)) {
                SherpaOnnxDecodeKeywordStream(g_keywordSpotter, g_keywordStream);
            }
            const SherpaOnnxKeywordResult* result = SherpaOnnxGetKeywordResult(g_keywordSpotter, g_keywordStream);
            if (result && result->keyword && strlen(result->keyword) > 0) {
                g_wakeWordDetected.store(true);
                PerformanceProfiler::End("Wake-Word Detection");
                OH_LOG_INFO(LOG_APP, "ohos_ASR wake‑word detected: %{public}s, start ASR processing", result->keyword);
                SherpaOnnxResetKeywordStream(g_keywordSpotter, g_keywordStream);
            }
            if (result) SherpaOnnxDestroyKeywordResult(result);

            if (!g_wakeWordDetected.load()) {
                continue;
            }
        }

        if (localBuf.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // VAD 路径：整句识别，无叠字
        static bool g_vadEnabled = true;
        static bool loggedPath = false;
        if (g_vadEnabled && g_vad) {
            if (!loggedPath) { OH_LOG_INFO(LOG_APP, "ohos_ASR PATH: VAD path"); loggedPath = true; }

            // 累积音频到至少 2560 samples (160ms) 再批量喂给 VAD
            // 避免零星 40ms 音频导致 VAD 将音节间短停顿误判为句子结束
            static std::vector<float> vadAccumulator;
            vadAccumulator.insert(vadAccumulator.end(), localBuf.begin(), localBuf.end());

            if (vadAccumulator.size() >= 2560) {
                OH_LOG_INFO(LOG_APP, "ohos_ASR VAD feed: accumulated %{public}zu samples",
                    vadAccumulator.size());

                auto vadStart = std::chrono::high_resolution_clock::now();
                SherpaOnnxVoiceActivityDetectorAcceptWaveform(g_vad, vadAccumulator.data(), (int32_t)vadAccumulator.size());
                auto vadEnd = std::chrono::high_resolution_clock::now();
                auto vadNs = std::chrono::duration_cast<std::chrono::nanoseconds>(vadEnd - vadStart).count();
                MetricsCollector::RecordVadLatency(std::max<int64_t>(vadNs / 1000, 1));
                vadAccumulator.clear();
            }

            // 处理 VAD 检测到的语音段（每个语音段视为一个完整句子）
            while (!SherpaOnnxVoiceActivityDetectorEmpty(g_vad) && g_isAsrRunning.load()) {
                const SherpaOnnxSpeechSegment* seg = SherpaOnnxVoiceActivityDetectorFront(g_vad);
                if (seg && seg->samples && seg->n > 0) {
                    // 在语音段后追加尾部静音padding（300ms），给解码器右上下文以正确处理段尾帧
                    std::vector<float> paddedAudio(seg->samples, seg->samples + seg->n);
                    const int trailingSilence = 4800; // 300ms @16kHz
                    paddedAudio.insert(paddedAudio.end(), trailingSilence, 0.0f);
                    SherpaOnnxOnlineStreamAcceptWaveform(g_asrStream, 16000,
                        paddedAudio.data(), (int32_t)paddedAudio.size());
                    // 标记输入结束，让解码器尽快输出
                    SherpaOnnxOnlineStreamInputFinished(g_asrStream);

                    // 解码
                    double segAudioDurationMs = (double)seg->n / 16000.0 * 1000.0;
                    auto segDecodeStart = std::chrono::steady_clock::now();
                    PerformanceProfiler::Start("ASR Segment Decode");
                    while (SherpaOnnxIsOnlineStreamReady(g_asrRecognizer, g_asrStream)) {
                        SherpaOnnxDecodeOnlineStream(g_asrRecognizer, g_asrStream);
                    }
                    PerformanceProfiler::End("ASR Segment Decode");
                    auto segDecodeEnd = std::chrono::steady_clock::now();
                    auto segDecodeMs = std::chrono::duration_cast<std::chrono::microseconds>(segDecodeEnd - segDecodeStart).count() / 1000.0;
                    double segRtf = segAudioDurationMs > 0 ? segDecodeMs / segAudioDurationMs : 0;
                    MetricsCollector::RecordAsrRtf(segRtf);
                    MetricsCollector::RecordAsrDecodeLatency((int64_t)segDecodeMs);
                    MetricsCollector::Record(MetricType::VadSpeechDurationMs, segAudioDurationMs);
                    MetricsCollector::RecordRingBufferFillRate(
                        (double)g_audioRingBuffer.Size() / (double)g_audioRingBuffer.Capacity());

                    // 获取最终结果
                    const SherpaOnnxOnlineRecognizerResult* r =
                        SherpaOnnxGetOnlineStreamResult(g_asrRecognizer, g_asrStream);
                    if (r && r->text && strlen(r->text) > 0) {
                        std::string currentText(r->text);
                        OH_LOG_INFO(LOG_APP, "ohos_ASR VAD result: seg=%{public}d samples(%.0fms) → \"%{public}s\"",
                            seg->n, segAudioDurationMs, currentText.c_str());
                OH_LOG_INFO(LOG_APP, "OHOS_load: VAD seg=%.0fms text=\"%{public}s\"",
                    segAudioDurationMs, currentText.c_str());
                        // 通过状态机处理 ASR 最终结果
                        g_audioStateMachine.OnAsrFinal(currentText);
                    } else {
                        OH_LOG_INFO(LOG_APP, "ohos_ASR segment result: EMPTY (r=%p, text=%p)", r, r ? r->text : nullptr);
                    }
                    if (r) SherpaOnnxDestroyOnlineRecognizerResult(r);

                    // 重置 stream，为下一个语音段做准备
                    SherpaOnnxOnlineStreamReset(g_asrRecognizer, g_asrStream);
                    lastText = "";
                    // 不清理 RingBuffer — VAD 已经消费了音频，剩余音频属于下一句
                    OH_LOG_INFO(LOG_APP, "ohos_ASR VAD segment done (ring not cleared)");
                }
                SherpaOnnxDestroySpeechSegment(seg);
                SherpaOnnxVoiceActivityDetectorPop(g_vad);
            }
        } else {
            if (!loggedPath) { OH_LOG_INFO(LOG_APP, "ohos_ASR PATH: Non-VAD path (may have 叠字)"); loggedPath = true; }
            // 无 VAD，直接喂入 ASR
            OH_LOG_INFO(LOG_APP, "ohos_ASR feeding %{public}zu samples to ASR stream", localBuf.size());
            SherpaOnnxOnlineStreamAcceptWaveform(g_asrStream, 16000, localBuf.data(), (int32_t)localBuf.size());

            // 计算音频时长(ms) = 样本数 / 16kHz * 1000
            double audioDurationMs = (double)localBuf.size() / 16000.0 * 1000.0;
            auto decodeStart = std::chrono::steady_clock::now();

            PerformanceProfiler::Start("ASR Partial Decode");
            while (SherpaOnnxIsOnlineStreamReady(g_asrRecognizer, g_asrStream)) {
                SherpaOnnxDecodeOnlineStream(g_asrRecognizer, g_asrStream);
            }
            PerformanceProfiler::End("ASR Partial Decode");

            auto decodeEnd = std::chrono::steady_clock::now();
            auto decodeMs = std::chrono::duration_cast<std::chrono::microseconds>(decodeEnd - decodeStart).count() / 1000.0;
            double rtf = audioDurationMs > 0 ? decodeMs / audioDurationMs : 0;
            MetricsCollector::RecordAsrRtf(rtf);
            MetricsCollector::RecordAsrDecodeLatency((int64_t)decodeMs);
            MetricsCollector::RecordRingBufferFillRate(
                (double)g_audioRingBuffer.Size() / (double)g_audioRingBuffer.Capacity());

            const SherpaOnnxOnlineRecognizerResult* r =
                SherpaOnnxGetOnlineStreamResult(g_asrRecognizer, g_asrStream);
            if (r && r->text && strlen(r->text) > 0) {
                std::string currentText(r->text);
                if (currentText != lastText) {
                    lastText = currentText;
                    OH_LOG_INFO(LOG_APP, "ohos_ASR partial: %{public}s", currentText.c_str());
                    // 中间结果 → 状态机转发到 ArkTS 显示
                    g_audioStateMachine.OnAsrInterim(currentText);
                }
            }
            if (r) SherpaOnnxDestroyOnlineRecognizerResult(r);

            // 端点检测
            if (SherpaOnnxOnlineStreamIsEndpoint(g_asrRecognizer, g_asrStream)) {
                OH_LOG_INFO(LOG_APP, "ohos_ASR endpoint TRIGGERED, lastText='%{public}s' len=%{public}zu",
                    lastText.c_str(), lastText.size());
                if (!lastText.empty()) {
                    OH_LOG_INFO(LOG_APP, "ohos_ASR endpoint: %{public}s", lastText.c_str());
                    g_audioStateMachine.OnAsrFinal(lastText);
                    lastText = "";
                    // 清除环形缓冲区残留，防止同一段语音被重复识别
                    g_audioRingBuffer.Clear();
                    OH_LOG_INFO(LOG_APP, "ohos_ASR cleared ring buffer after endpoint");
                }
                SherpaOnnxOnlineStreamReset(g_asrRecognizer, g_asrStream);
            }
        }
    }
    OH_LOG_INFO(LOG_APP, "ohos_ASR BackgroundAsrThread finished (isRunning=%{public}d)", g_isAsrRunning.load());
}

// --- NAPI: setDuplexCallbacks(callback) ---
// 注册全双工 Action 回调，状态机通过此回调通知 ArkTS 执行动作
static napi_value SetDuplexCallbacks(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        OH_LOG_ERROR(LOG_APP, "ohos_DUPLEX: need 1 arg (callback)");
        return nullptr;
    }

    // 释放旧的 TSFN
    if (g_duplexActionTsFunction) {
        napi_release_threadsafe_function(g_duplexActionTsFunction, napi_tsfn_release);
        g_duplexActionTsFunction = nullptr;
    }

    napi_value resource_name;
    napi_create_string_utf8(env, "duplexActionCallback", NAPI_AUTO_LENGTH, &resource_name);
    napi_create_threadsafe_function(env, argv[0], nullptr, resource_name,
        0, 1, nullptr, nullptr, nullptr, DuplexActionCallJsCallback, &g_duplexActionTsFunction);

    OH_LOG_INFO(LOG_APP, "ohos_DUPLEX: action callbacks registered");
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// --- NAPI: startAssistant(asrModelDir, vadModelPath, kwsModelPath, callback) ---
static napi_value StartAssistant(napi_env env, napi_callback_info info) {
    auto saStart = std::chrono::steady_clock::now();
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        OH_LOG_ERROR(LOG_APP, "ohos_ASR need 4 args (asrModelDir, vadModelPath, kwsModelPath, callback)");
        return nullptr;
    }

    // 解析参数
    size_t len;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string asrModelDir(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &asrModelDir[0], len + 1, &len);
    asrModelDir.resize(len);
    napi_get_value_string_utf8(env, argv[1], nullptr, 0, &len);
    std::string vadModelPath(len, '\0');
    napi_get_value_string_utf8(env, argv[1], &vadModelPath[0], len + 1, &len);
    vadModelPath.resize(len);
    napi_get_value_string_utf8(env, argv[2], nullptr, 0, &len);
    std::string kwsModelPath(len, '\0');
    napi_get_value_string_utf8(env, argv[2], &kwsModelPath[0], len + 1, &len);
    kwsModelPath.resize(len);

    bool prewarmMode = (kwsModelPath == "__prewarm__");
    OH_LOG_INFO(LOG_APP, "ohos_ASR StartAssistant: prewarmMode=%{public}d prewarmed=%{public}d",
        prewarmMode, g_asrPrewarmed.load());

    OH_LOG_INFO(LOG_APP, "ohos_ASR asrModelDir=%{public}s, vadModel=%{public}s, kwsModel=%{public}s",
                asrModelDir.c_str(), vadModelPath.c_str(), kwsModelPath.c_str());

    // --- 初始化 AEC 回声消除 ---
    if (!g_aec) {
        g_aec = std::make_unique<AcousticEchoCanceller>();
    }

    // 预热模式：仅创建引擎不启动线程
    if (prewarmMode) {
        OH_LOG_INFO(LOG_APP, "ohos_ASR PREWARM mode: loading engine without starting threads");
    }

    // --- 初始化 ASR 引擎 ---
    std::string encoderPath = asrModelDir + "/encoder-epoch-99-avg-1.int8.onnx";
    std::string decoderPath = asrModelDir + "/decoder-epoch-99-avg-1.int8.onnx";
    std::string joinerPath = asrModelDir + "/joiner-epoch-99-avg-1.onnx";
    std::string tokensPath = asrModelDir + "/tokens.txt";

    SherpaOnnxOnlineTransducerModelConfig transducer;
    memset(&transducer, 0, sizeof(transducer));
    transducer.encoder = encoderPath.c_str();
    transducer.decoder = decoderPath.c_str();
    transducer.joiner = joinerPath.c_str();

    SherpaOnnxOnlineModelConfig modelConfig;
    memset(&modelConfig, 0, sizeof(modelConfig));
    modelConfig.transducer = transducer;
    modelConfig.tokens = tokensPath.c_str();
    modelConfig.num_threads = 2;
    modelConfig.debug = 1;
    modelConfig.provider = "cpu";
    modelConfig.model_type = "";

    SherpaOnnxFeatureConfig featConfig;
    memset(&featConfig, 0, sizeof(featConfig));
    featConfig.sample_rate = 16000;
    featConfig.feature_dim = 80;

    SherpaOnnxOnlineRecognizerConfig recognizerConfig;
    memset(&recognizerConfig, 0, sizeof(recognizerConfig));
    recognizerConfig.feat_config = featConfig;
    recognizerConfig.model_config = modelConfig;
    recognizerConfig.decoding_method = "modified_beam_search";
    recognizerConfig.max_active_paths = 8;
    recognizerConfig.enable_endpoint = 1;
    recognizerConfig.rule1_min_trailing_silence = 1.5f;
    recognizerConfig.rule2_min_trailing_silence = 2.5f;
    recognizerConfig.rule3_min_utterance_length = 20.0f;

    OH_LOG_INFO(LOG_APP, "ohos_ASR creating recognizer: method=%{public}s, max_paths=%{public}d, threads=%{public}d",
        recognizerConfig.decoding_method, recognizerConfig.max_active_paths,
        recognizerConfig.model_config.num_threads);

    // 如果已预热，跳过模型引擎创建
    if (!g_asrPrewarmed.load() || prewarmMode) {

    // 检查所有模型文件是否存在
    auto checkFile = [](const std::string& path, const char* name) -> bool {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            OH_LOG_ERROR(LOG_APP, "ohos_ASR MISSING MODEL FILE: %{public}s at %{public}s", name, path.c_str());
            return false;
        }
        fclose(f);
        OH_LOG_INFO(LOG_APP, "ohos_ASR model file found: %{public}s -> %{public}s", name, path.c_str());
        return true;
    };
    if (!checkFile(encoderPath, "encoder")) {
        napi_throw_error(env, nullptr, ("Missing encoder model: " + encoderPath).c_str());
        return nullptr;
    }
    if (!checkFile(decoderPath, "decoder")) {
        napi_throw_error(env, nullptr, ("Missing decoder model: " + decoderPath).c_str());
        return nullptr;
    }
    if (!checkFile(joinerPath, "joiner")) {
        napi_throw_error(env, nullptr, ("Missing joiner model: " + joinerPath).c_str());
        return nullptr;
    }
    if (!checkFile(tokensPath, "tokens")) {
        napi_throw_error(env, nullptr, ("Missing tokens file: " + tokensPath).c_str());
        return nullptr;
    }

    g_asrRecognizer = SherpaOnnxCreateOnlineRecognizer(&recognizerConfig);
    if (!g_asrRecognizer) {
        OH_LOG_ERROR(LOG_APP, "ohos_ASR SherpaOnnxCreateOnlineRecognizer FAILED");
        napi_throw_error(env, nullptr, "SherpaOnnxCreateOnlineRecognizer FAILED - model files may be invalid");
        return nullptr;
    }
    OH_LOG_INFO(LOG_APP, "ohos_ASR recognizer created OK");

    // --- 初始化 Keyword Spotter（仅当提供了关键词文件路径时） ---
    if (!kwsModelPath.empty()) {
        SherpaOnnxOnlineModelConfig kwsModelConfig;
        memset(&kwsModelConfig, 0, sizeof(kwsModelConfig));
        kwsModelConfig.transducer = transducer;
        kwsModelConfig.tokens = tokensPath.c_str();
        kwsModelConfig.num_threads = 1;
        kwsModelConfig.debug = 1;
        kwsModelConfig.provider = "cpu";
        kwsModelConfig.model_type = "zipformer";

        SherpaOnnxKeywordSpotterConfig kwConfig;
        memset(&kwConfig, 0, sizeof(kwConfig));
        kwConfig.feat_config = featConfig;
        kwConfig.model_config = kwsModelConfig;
        kwConfig.max_active_paths = 4;
        kwConfig.keywords_file = kwsModelPath.c_str();
        kwConfig.keywords_score = 1.0f;
        kwConfig.keywords_threshold = 0.25f;

        OH_LOG_INFO(LOG_APP, "ohos_ASR creating keyword spotter with model: %{public}s", kwsModelPath.c_str());
        
        // --- 调试：打印关键词文件内容 ---
        FILE* fKws = fopen(kwsModelPath.c_str(), "r");
        if (fKws) {
            char buf[256];
            while (fgets(buf, sizeof(buf), fKws)) {
                OH_LOG_INFO(LOG_APP, "ohos_ASR Keyword file line: %{public}s", buf);
            }
            fclose(fKws);
        } else {
            OH_LOG_ERROR(LOG_APP, "ohos_ASR Keyword file NOT accessible: %{public}s", kwsModelPath.c_str());
        }

        g_keywordSpotter = SherpaOnnxCreateKeywordSpotter(&kwConfig);
        if (g_keywordSpotter) {
            g_keywordStream = SherpaOnnxCreateKeywordStream(g_keywordSpotter);
            OH_LOG_INFO(LOG_APP, "ohos_ASR keyword spotter created OK");
        } else {
            OH_LOG_ERROR(LOG_APP, "ohos_ASR keyword spotter creation FAILED");
        }
    } else {
        OH_LOG_INFO(LOG_APP, "ohos_ASR kwsModelPath is empty, skipping keyword spotter creation");
        g_keywordSpotter = nullptr;
        g_keywordStream = nullptr;
    }

    g_asrStream = const_cast<SherpaOnnxOnlineStream*>(
        SherpaOnnxCreateOnlineStream(g_asrRecognizer));
    OH_LOG_INFO(LOG_APP, "ohos_ASR stream created OK");

    // --- 初始化 VAD（可选）---
    g_vad = nullptr;
    FILE* fVad = fopen(vadModelPath.c_str(), "rb");
    if (fVad) {
        fclose(fVad);
        OH_LOG_INFO(LOG_APP, "ohos_ASR creating silero VAD...");
        SherpaOnnxSileroVadModelConfig sileroConfig;
        memset(&sileroConfig, 0, sizeof(sileroConfig));
        sileroConfig.model = vadModelPath.c_str();
        // 阈值 0.3 足够敏感
        sileroConfig.threshold = 0.3f;
        // 最短语音段 0.3s（防单音节误切）
        sileroConfig.min_speech_duration = 0.3f;
        // 静音判定 0.5s（用户停止说话后快速触发 ASR）
        sileroConfig.min_silence_duration = 0.5f;
        sileroConfig.window_size = 512;
        sileroConfig.max_speech_duration = 30.0f;

        OH_LOG_INFO(LOG_APP, "ohos_ASR VAD config: threshold=%.1f, min_speech=%.2f, min_silence=%.2f",
            sileroConfig.threshold, sileroConfig.min_speech_duration, sileroConfig.min_silence_duration);

        SherpaOnnxVadModelConfig vadConfig;
        memset(&vadConfig, 0, sizeof(vadConfig));
        vadConfig.silero_vad = sileroConfig;
        vadConfig.sample_rate = 16000;
        vadConfig.num_threads = 1;
        vadConfig.provider = "cpu";
        vadConfig.debug = 1;

        g_vad = SherpaOnnxCreateVoiceActivityDetector(&vadConfig, 30.0f);
        if (g_vad) {
            OH_LOG_INFO(LOG_APP, "ohos_ASR VAD created OK");
        } else {
            OH_LOG_WARN(LOG_APP, "ohos_ASR VAD creation failed, running without VAD");
        }
    } else {
        OH_LOG_INFO(LOG_APP, "ohos_ASR no VAD model found, running without VAD");
    }
    } // end prewarm skip

    // --- 引擎恢复路径：Recognizer 存活但 stream/VAD 被销毁时快速重建 ---
    if (g_asrRecognizer && !g_asrStream) {
        g_asrStream = const_cast<SherpaOnnxOnlineStream*>(
            SherpaOnnxCreateOnlineStream(g_asrRecognizer));
        OH_LOG_INFO(LOG_APP, "ohos_ASR stream recreated (fast recovery path)");
    }
    if (g_asrRecognizer && !g_vad && !vadModelPath.empty()) {
        FILE* fVad = fopen(vadModelPath.c_str(), "rb");
        if (fVad) {
            fclose(fVad);
            SherpaOnnxSileroVadModelConfig sileroConfig;
            memset(&sileroConfig, 0, sizeof(sileroConfig));
            sileroConfig.model = vadModelPath.c_str();
            sileroConfig.threshold = 0.3f;
            sileroConfig.min_speech_duration = 0.3f;
            sileroConfig.min_silence_duration = 0.5f;
            sileroConfig.window_size = 512;
            sileroConfig.max_speech_duration = 30.0f;

            SherpaOnnxVadModelConfig vadConfig;
            memset(&vadConfig, 0, sizeof(vadConfig));
            vadConfig.silero_vad = sileroConfig;
            vadConfig.sample_rate = 16000;
            vadConfig.num_threads = 1;
            vadConfig.provider = "cpu";
            vadConfig.debug = 1;

            g_vad = SherpaOnnxCreateVoiceActivityDetector(&vadConfig, 30.0f);
            OH_LOG_INFO(LOG_APP, "ohos_ASR VAD recreated (fast recovery path)");
        }
    }

    // --- 创建 TSFN 回调（预热模式不创建 TSFN 和线程）---
    if (prewarmMode) {
        // 算子预热：喂一段静音让 CPU/NPU 缓存加载模型权重
        if (g_asrRecognizer && g_asrStream) {
            OH_LOG_INFO(LOG_APP, "OHOS_load: ASR warmup inference start");
            std::vector<float> dummyAudio(3200, 0.0f); // 200ms 静音 @16kHz
            SherpaOnnxOnlineStreamAcceptWaveform(g_asrStream, 16000, dummyAudio.data(), (int32_t)dummyAudio.size());
            SherpaOnnxOnlineStreamInputFinished(g_asrStream);
            while (SherpaOnnxIsOnlineStreamReady(g_asrRecognizer, g_asrStream)) {
                SherpaOnnxDecodeOnlineStream(g_asrRecognizer, g_asrStream);
            }
            // 丢弃预热结果
            const auto* warmupResult = SherpaOnnxGetOnlineStreamResult(g_asrRecognizer, g_asrStream);
            if (warmupResult) SherpaOnnxDestroyOnlineRecognizerResult(warmupResult);
            // 重置 stream 给后续真实使用
            SherpaOnnxOnlineStreamReset(g_asrRecognizer, g_asrStream);
            OH_LOG_INFO(LOG_APP, "OHOS_load: ASR warmup done");
        }
        g_asrPrewarmed.store(true);
        auto saEnd = std::chrono::steady_clock::now();
        auto saMs = std::chrono::duration_cast<std::chrono::milliseconds>(saEnd - saStart).count();
        OH_LOG_INFO(LOG_APP, "OHOS_load: ASR prewarm done in %{public}lld ms", (long long)saMs);
        napi_value result;
        napi_get_undefined(env, &result);
        return result;
    }

    // TSFN 已在 SetDuplexCallbacks 中注册，ASR 结果通过状态机 Action 回调传递

    // 重置 FeedAudio 内部状态（DC 滤波器、AEC 计数器、静默保护期等）
    ResetFeedAudioState();

    // 清空环形缓冲区，启动解码线程
    // 先确保旧线程已退出，再启动新线程
    g_wakeWordDetected.store(false);
    if (g_asrThread.joinable()) {
        g_isAsrRunning.store(false);
        g_asrThread.join();
    }
    g_isAsrRunning.store(true);
    g_asrThread = std::thread(BackgroundAsrThread);
    OH_LOG_INFO(LOG_APP, "ohos_ASR Assistant started, background thread launched");
    auto saEnd = std::chrono::steady_clock::now();
    auto saMs = std::chrono::duration_cast<std::chrono::milliseconds>(saEnd - saStart).count();
    OH_LOG_INFO(LOG_APP, "OHOS_load: ASR init done in %{public}lld ms", (long long)saMs);

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// --- NAPI: feedAudio(pcmData: ArrayBuffer) ---
static napi_value FeedAudio(napi_env env, napi_callback_info info) {
    if (!g_isAsrRunning.load()) return nullptr;

    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return nullptr;

    // 获取 ArrayBuffer 中的 int16 PCM 数据
    void* data = nullptr;
    size_t byteLength = 0;
    napi_get_arraybuffer_info(env, argv[0], &data, &byteLength);
    if (!data || byteLength == 0) return nullptr;

    // int16 → float32 归一化
    int16_t* pcm = static_cast<int16_t*>(data);
    size_t sampleCount = byteLength / sizeof(int16_t);

    std::vector<float> floatSamples(sampleCount);
    // DC 阻挡高通滤波器：一阶 IIR，截止频率 ~40Hz @16kHz
    // y[n] = x[n] - x[n-1] + 0.95 * y[n-1]
    for (size_t i = 0; i < sampleCount; i++) {
        float x = (float)pcm[i] / 32768.0f;
        float y = x - g_feedAudioState.prevInput + 0.95f * g_feedAudioState.prevOutput;
        g_feedAudioState.prevInput = x;
        g_feedAudioState.prevOutput = y;
        floatSamples[i] = y;
    }

    // === 1. AEC 回声消除（先于状态机，防止回声误判为打断）===
    bool ttsPlaying = g_audioStateMachine.IsTtsPlaying();
    bool aecReady = false;
    if (ttsPlaying && g_aec && g_aec->IsActive()) {
        g_aec->ProcessMicAudio(floatSamples.data(), sampleCount);
        aecReady = g_aec->IsConverged();
        if (++g_feedAudioState.aecLogCounter % 5 == 0) {
            OH_LOG_INFO(LOG_APP, "ohos_AEC: converged=%{public}d ref=%{public}d",
                g_aec->IsConverged(), g_aec->HasEnoughReference());
        }
    }

    // === 2. TTS 播放结束后的静默保护期（防止回声残留被 ASR 捕获）===
    bool ttsJustEnded = g_feedAudioState.prevTtsPlaying && !ttsPlaying;
    if (ttsJustEnded) {
        g_feedAudioState.ttsEndTime = std::chrono::steady_clock::now();
        OH_LOG_INFO(LOG_APP, "ohos_AEC: TTS ended, mute period started");
    }
    g_feedAudioState.prevTtsPlaying = ttsPlaying;
    bool inMutePeriod = false;
    if (!ttsPlaying && !ttsJustEnded) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_feedAudioState.ttsEndTime).count();
        inMutePeriod = (elapsed < 300); // 300ms 静默保护
    }

    // === 3. 全双工模式：喂给 AudioStateMachine 做打断检测 ===
    // 注意：仅在 AEC 已收敛或非 TTS 播放时检测，防止回声误判为打断
    if (g_audioStateMachine.IsRunning()) {
        bool wasTtsPlaying = g_audioStateMachine.IsTtsPlaying();
        if (!ttsPlaying || aecReady) {
            g_audioStateMachine.FeedAudio(floatSamples.data(), floatSamples.size());
        }
        if (wasTtsPlaying && !g_audioStateMachine.IsTtsPlaying()) {
            OH_LOG_INFO(LOG_APP, "ohos_FD INTERRUPTION: state left TTS_PLAYING");
        }
    }

    // === 4. 喂给 ASR（AEC 收敛 + 非静默保护期）===
    if ((!ttsPlaying || aecReady) && !inMutePeriod) {
        g_audioRingBuffer.Write(floatSamples.data(), sampleCount);
    }

    // 记录缓冲区水位（每 20 帧采样一次，避免高频记录）
    if (++g_feedAudioState.feedCounter % 20 == 0) {
        MetricsCollector::RecordRingBufferFillRate(
            (double)g_audioRingBuffer.Size() / (double)g_audioRingBuffer.Capacity());
        MetricsCollector::Record(MetricType::AudioChunkSize, (double)sampleCount);
    }

    return nullptr;
}

// --- NAPI: stopAssistant ---
static napi_value StopAssistant(napi_env env, napi_callback_info info) {
    OH_LOG_INFO(LOG_APP, "ohos_ASR StopAssistant called");
    g_isAsrRunning.store(false);
    if (g_asrThread.joinable()) g_asrThread.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 重置 AEC 状态
    if (g_aec) {
        g_aec->Reset();
        g_aec.reset();
    }

    // 销毁常驻 TTS 引擎
    {
        std::lock_guard<std::mutex> lock(g_ttsMutex);
        if (g_ttsEngine) {
            SherpaOnnxDestroyOfflineTts(g_ttsEngine);
            g_ttsEngine = nullptr;
            g_ttsModelDir.clear();
            OH_LOG_INFO(LOG_APP, "ohos_TTS_ENGINE: destroyed");
        }
    }

    // ASR 结果通过状态机 Action 回调传递，无需独立 TSFN

    if (g_asrStream) {
        SherpaOnnxDestroyOnlineStream(g_asrStream);
        g_asrStream = nullptr;
    }
    // 保留 g_asrRecognizer 不销毁，下次全双工重开时只需重建 stream（~0ms）
    // g_asrPrewarmed 保持 true，确保 StartAssistant 跳过引擎初始化
    if (g_vad) {
        SherpaOnnxDestroyVoiceActivityDetector(g_vad);
        g_vad = nullptr;
    }
    if (g_keywordStream) {
        SherpaOnnxDestroyOnlineStream(g_keywordStream);
        g_keywordStream = nullptr;
    }
    if (g_keywordSpotter) {
        SherpaOnnxDestroyKeywordSpotter(g_keywordSpotter);
        g_keywordSpotter = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_audioBufMutex);
        g_audioBuffer.clear();
    }
    g_audioRingBuffer.Clear();

    OH_LOG_INFO(LOG_APP, "ohos_ASR Assistant stopped and cleaned up");
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// --- 回调上下文结构体 ---
struct TtsCallbackContext {
    napi_threadsafe_function tsfn;
    std::atomic<bool>* isGenerating;
    std::string text;
    std::string modelDir;
    float speed;
};

// --- 传递给 JS 回调的音频数据 ---
struct TtsAudioChunk {
    int16_t* pcmData;   // PCM int16 数据（由 new[] 分配）
    int32_t sampleCount;
    float progress;
    int32_t sampleRate;  // 模型的实际采样率
};

static bool FileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        OH_LOG_ERROR(LOG_APP, "ohos_TTS FileExists: NOT found: %{public}s", path.c_str());
        return false;
    }
    fclose(f);
    OH_LOG_INFO(LOG_APP, "ohos_TTS FileExists: found: %{public}s", path.c_str());
    return true;
}

// --- 流式 TTS 的回调上下文 ---
struct TtsStreamingCtx {
    napi_threadsafe_function tsfn;
    int32_t sampleRate;
    std::atomic<int>* chunkCount;
};

// sherpa-onnx 逐句回调：每生成一句话触发一次
static void TtsSentenceCallback(const SherpaOnnxGeneratedAudio* audio, const char* text) {
    if (!audio || !audio->samples || audio->n <= 0) return;
    // 从回调上下文获取 TSFN（通过全局变量传递，简化实现）
    // 实际使用中通过回调 arg 传递，这里用全局变量
}

// 用全局变量传递 TSFN 给回调
static napi_threadsafe_function g_streamingTsfn = nullptr;
static std::atomic<int> g_streamingChunkCount{0};
// 共享 TTS 流式回调 TSFN（所有句子共用，确保回调顺序）
static napi_threadsafe_function g_ttsSharedTsFunction = nullptr;
static std::atomic<int> g_ttsActiveThreads{0};
static std::chrono::steady_clock::time_point g_generateStartTime;
static int32_t g_streamingSampleRate = 0;
static std::chrono::steady_clock::time_point g_firstChunkTime;
static std::atomic<bool> g_firstChunkArrived{false};
// AEC 参考信号降采样相位累加器（44100Hz → 16000Hz）
static int64_t g_aecRefPhaseAccum = 0;

// sherpa-onnx 逐句回调：每生成一句话触发一次
// 返回 1 继续生成，返回 0 停止（sherpa-onnx 约定）
static int32_t StreamingTtsCallback(const float* samples, int32_t n) {
    if (!samples || n <= 0) return 1;

    auto now = std::chrono::steady_clock::now();
    if (!g_firstChunkArrived.exchange(true)) {
        g_firstChunkTime = now;
        OH_LOG_INFO(LOG_APP, "ohos_TTS_TIMING: first chunk at +0ms");
        auto elapsedFirst = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_generateStartTime).count();
        OH_LOG_INFO(LOG_APP, "OHOS_TTS_PERF 06_CXX_firstChunk tid=%{public}d afterGenerate=%{public}lldms n=%{public}d sr=%{public}d",
                    (int)gettid(), (long long)elapsedFirst, n, g_streamingSampleRate);
        g_generateStartTime = now;  // 记录当前时间给后续 chunk 用
        // 但后续 chunk 使用 g_generateStartTime 已过时，改用绝对时间比较
    } else {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_firstChunkTime).count();
        OH_LOG_INFO(LOG_APP, "ohos_TTS_TIMING: chunk #%{public}d at +%{public}lld ms, %{public}d samples",
                    g_streamingChunkCount.load() + 1, (long long)elapsed, n);
    }

    int32_t sr = g_streamingSampleRate;
    TtsAudioChunk* chunk = new TtsAudioChunk;
    chunk->pcmData = new int16_t[n];
    chunk->sampleCount = n;
    chunk->sampleRate = sr;
    chunk->progress = 0.5f;

    for (int32_t i = 0; i < n; i++) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        chunk->pcmData[i] = (int16_t)(s * 32767.0f);
    }

    // === AEC: 记录 TTS 参考信号（需降采样到 16000Hz 以匹配 ASR 采样率）===
    if (g_aec && g_aec->IsActive() && sr > 0 && n > 0) {
        std::vector<float> aecRefBuf;
        // 降采样后的预估大小，防止 n*16000 溢出 int32
        size_t estimatedOutSize = static_cast<size_t>(n) * 16000ULL / static_cast<size_t>(sr) + 1;
        if (estimatedOutSize > 0 && estimatedOutSize < 1000000) {
            aecRefBuf.reserve(estimatedOutSize);
        }
        for (int32_t i = 0; i < n; i++) {
            g_aecRefPhaseAccum += 16000;
            if (g_aecRefPhaseAccum >= sr) {
                g_aecRefPhaseAccum -= sr;
                aecRefBuf.push_back(samples[i]);
            }
        }
        if (!aecRefBuf.empty()) {
            g_aec->AddTtsReference(aecRefBuf.data(), aecRefBuf.size());
        }
    }

    g_streamingChunkCount.fetch_add(1);
    OH_LOG_INFO(LOG_APP, "ohos_TTS streaming chunk #%{public}d: %{public}d samples",
                g_streamingChunkCount.load(), n);
    OH_LOG_INFO(LOG_APP, "OHOS_fixbug_tts CXX_chunk tid=%{public}d #%{public}d n=%{public}d sr=%{public}d",
                (int)gettid(), g_streamingChunkCount.load(), n, sr);

    if (g_streamingTsfn) {
        napi_call_threadsafe_function(g_streamingTsfn, chunk, napi_tsfn_nonblocking);
    } else {
        delete[] chunk->pcmData;
        delete chunk;
    }
    return 1; // 继续生成下一句（sherpa-onnx 约定：1=继续，0=停止）
}

// 初始化或复用 TTS 引擎
static bool EnsureTtsEngine(const std::string& modelDir) {
    std::lock_guard<std::mutex> lock(g_ttsMutex);
    if (g_ttsEngine && g_ttsModelDir == modelDir) {
        return true; // 引擎已存在且模型未变
    }
    // 销毁旧引擎
    if (g_ttsEngine) {
        SherpaOnnxDestroyOfflineTts(g_ttsEngine);
        g_ttsEngine = nullptr;
    }
    // 检查模型文件
    std::string modelPath = modelDir + "/model.onnx";
    std::string tokensPath = modelDir + "/tokens.txt";
    std::string lexiconPath = modelDir + "/lexicon.txt";
    bool hasLexicon = FileExists(lexiconPath);
    if (!FileExists(modelPath) || !FileExists(tokensPath)) {
        OH_LOG_ERROR(LOG_APP, "ohos_TTS_ENGINE: model files not found in %{public}s", modelDir.c_str());
        return false;
    }
    // 创建引擎
    SherpaOnnxOfflineTtsVitsModelConfig vitsModelConfig;
    memset(&vitsModelConfig, 0, sizeof(vitsModelConfig));
    vitsModelConfig.model = modelPath.c_str();
    vitsModelConfig.tokens = tokensPath.c_str();
    vitsModelConfig.lexicon = hasLexicon ? lexiconPath.c_str() : "";
    vitsModelConfig.data_dir = "";
    vitsModelConfig.noise_scale = 0.667f;
    vitsModelConfig.noise_scale_w = 0.8f;
    vitsModelConfig.length_scale = 1.0f;

    SherpaOnnxOfflineTtsModelConfig modelConfig;
    memset(&modelConfig, 0, sizeof(modelConfig));
    modelConfig.vits = vitsModelConfig;
    modelConfig.num_threads = 2;
    modelConfig.debug = 1;
    modelConfig.provider = "cpu";

    SherpaOnnxOfflineTtsConfig ttsConfig;
    memset(&ttsConfig, 0, sizeof(ttsConfig));
    ttsConfig.model = modelConfig;
    ttsConfig.max_num_sentences = 0;

    auto t0 = std::chrono::steady_clock::now();
    g_ttsEngine = SherpaOnnxCreateOfflineTts(&ttsConfig);
    auto t1 = std::chrono::steady_clock::now();
    auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (!g_ttsEngine) {
        OH_LOG_ERROR(LOG_APP, "ohos_TTS_ENGINE: SherpaOnnxCreateOfflineTts FAILED");
        return false;
    }
    g_ttsModelDir = modelDir;
    OH_LOG_INFO(LOG_APP, "ohos_TTS_ENGINE: created in %{public}lld ms, sr=%{public}d",
                (long long)loadMs, SherpaOnnxOfflineTtsSampleRate(g_ttsEngine));
    return true;
}
void BackgroundTtsThread(void* data) {
    TtsCallbackContext* ctx = static_cast<TtsCallbackContext*>(data);
    ctx->isGenerating->store(true);

    const std::string& text = ctx->text;
    const std::string& modelDir = ctx->modelDir;
    float speed = ctx->speed;

    OH_LOG_INFO(LOG_APP, "ohos_TTS BackgroundThread: text=%{public}s, modelDir=%{public}s, speed=%.1f",
                text.c_str(), modelDir.c_str(), speed);
    OH_LOG_INFO(LOG_APP, "OHOS_fixbug_tts CXX_start tid=%{public}d text=\"%{public}s\"",
                (int)gettid(), text.c_str());
    auto t0_generate = std::chrono::steady_clock::now();
    OH_LOG_INFO(LOG_APP, "OHOS_TTS_PERF 05_CXX_startGenerate tid=%{public}d", (int)gettid());

    // 使用常驻引擎（首次创建，后续复用，消除 2 秒模型加载延迟）
    if (!EnsureTtsEngine(modelDir)) {
        OH_LOG_ERROR(LOG_APP, "ohos_TTS engine init failed");
        ctx->isGenerating->store(false);
        napi_call_threadsafe_function(ctx->tsfn, nullptr, napi_tsfn_nonblocking);
        napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
        delete ctx->isGenerating;
        delete ctx;
        return;
    }

    g_ttsMutex.lock();
    const SherpaOnnxOfflineTts* tts = g_ttsEngine;

    g_streamingSampleRate = SherpaOnnxOfflineTtsSampleRate(tts);
    g_streamingChunkCount.store(0);
    g_streamingTsfn = ctx->tsfn;
    g_firstChunkArrived.store(false);
    g_aecRefPhaseAccum = 0;
    g_generateStartTime = std::chrono::steady_clock::now();

    OH_LOG_INFO(LOG_APP, "ohos_TTS generating streaming for: %{public}s", text.c_str());
    PerformanceProfiler::Start("TTS Generation");
    auto ttsStart = std::chrono::steady_clock::now();

    const SherpaOnnxGeneratedAudio* fullAudio =
        SherpaOnnxOfflineTtsGenerateWithCallback(tts, text.c_str(), 0, speed,
            StreamingTtsCallback);

    auto ttsEnd = std::chrono::steady_clock::now();
    auto ttsMs = std::chrono::duration_cast<std::chrono::milliseconds>(ttsEnd - ttsStart).count();
    PerformanceProfiler::End("TTS Generation");
    MetricsCollector::RecordTtsGeneration(ttsMs);
    g_ttsMutex.unlock();

    // 发送完成标志
    TtsAudioChunk* done = new TtsAudioChunk;
    done->pcmData = nullptr;
    done->sampleCount = 0;
    done->progress = 2.0f;
    done->sampleRate = g_streamingSampleRate;
    napi_call_threadsafe_function(ctx->tsfn, done, napi_tsfn_blocking);

    OH_LOG_INFO(LOG_APP, "ohos_TTS streaming done: %{public}d chunks, %{public}lld ms",
                g_streamingChunkCount.load(), (long long)ttsMs);
    OH_LOG_INFO(LOG_APP, "OHOS_fixbug_tts CXX_done tid=%{public}d chunks=%{public}d",
                (int)gettid(), g_streamingChunkCount.load());

    if (fullAudio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(fullAudio);
    // 不销毁引擎，复用
    g_streamingTsfn = nullptr;
    ctx->isGenerating->store(false);
    // 最后一个线程释放共享 TSFN
    if (g_ttsActiveThreads.fetch_sub(1) <= 1 && g_ttsSharedTsFunction) {
        napi_release_threadsafe_function(g_ttsSharedTsFunction, napi_tsfn_release);
        g_ttsSharedTsFunction = nullptr;
        OH_LOG_INFO(LOG_APP, "ohos_TTS: shared TSFN released");
    }
    delete ctx->isGenerating;
    delete ctx;
    OH_LOG_INFO(LOG_APP, "ohos_TTS BackgroundThread finished");
}

// --- 4. TSFN 回调 (JS 侧执行，流式版) ---
void CallJsCallback(napi_env env, napi_value jsCallback, void* context, void* data) {
    if (!data) return;
    TtsAudioChunk* chunk = static_cast<TtsAudioChunk*>(data);

    auto jsStart = std::chrono::steady_clock::now();
    static auto lastJsTime = jsStart;
    static bool firstJsCall = true;

    if (chunk->pcmData && chunk->sampleCount > 0) {
        if (firstJsCall) {
            OH_LOG_INFO(LOG_APP, "ohos_TTS_TIMING: JS dispatch first chunk at +0ms");
            firstJsCall = false;
        } else {
            auto sinceLast = std::chrono::duration_cast<std::chrono::microseconds>(jsStart - lastJsTime).count();
            OH_LOG_INFO(LOG_APP, "ohos_TTS_TIMING: JS dispatch chunk, gap=%{public}lld us, bytes=%{public}zu",
                        (long long)sinceLast, chunk->sampleCount * sizeof(int16_t));
        }
        lastJsTime = jsStart;
    }

    napi_value undefined;
    napi_get_undefined(env, &undefined);

    napi_value jsArrayBuffer;
    napi_value jsProgress;
    napi_value jsSampleRate;

    if (chunk->pcmData && chunk->sampleCount > 0) {
        // 正常音频块
        void* arrayBufferData = nullptr;
        size_t byteLength = chunk->sampleCount * sizeof(int16_t);
        napi_create_arraybuffer(env, byteLength, &arrayBufferData, &jsArrayBuffer);
        memcpy(arrayBufferData, chunk->pcmData, byteLength);
        napi_create_double(env, (double)chunk->progress, &jsProgress);
        delete[] chunk->pcmData;
    } else if (chunk->progress >= 2.0f) {
        // 完成标志: 发送空 ArrayBuffer + progress=2.0
        napi_create_arraybuffer(env, 0, nullptr, &jsArrayBuffer);
        napi_create_double(env, 2.0, &jsProgress);
    } else {
        // 错误或空数据
        napi_create_arraybuffer(env, 0, nullptr, &jsArrayBuffer);
        napi_create_double(env, -1.0, &jsProgress);
    }
    napi_create_int32(env, chunk->sampleRate, &jsSampleRate);

    napi_value argv[3] = { jsArrayBuffer, jsProgress, jsSampleRate };
    napi_call_function(env, undefined, jsCallback, 3, argv, nullptr);
    delete chunk;
}

// --- 5. NAPI 导出函数 ---

// pingNative 实现
static napi_value PingNative(napi_env env, napi_callback_info info) {
    napi_value result;
    napi_create_int32(env, 42, &result); // 示例返回值
    return result;
}

// startStreamingTtsWithSpeed(text, modelDir, speed, callback)
static napi_value StartStreamingTtsWithSpeed(napi_env env, napi_callback_info info) {
    OH_LOG_INFO(LOG_APP, "ohos_tsswithspeed called.");
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) return nullptr;
    // argv[0]: text, argv[1]: modelDir, argv[2]: speed, argv[3]: jsCallback
    size_t textLen;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &textLen);
    std::string text(textLen, '\0');
    napi_get_value_string_utf8(env, argv[0], &text[0], textLen + 1, &textLen);
    text.resize(textLen);
    size_t modelDirLen;
    napi_get_value_string_utf8(env, argv[1], nullptr, 0, &modelDirLen);
    std::string modelDir(modelDirLen, '\0');
    napi_get_value_string_utf8(env, argv[1], &modelDir[0], modelDirLen + 1, &modelDirLen);
    modelDir.resize(modelDirLen);

    // 空文本 = 预热模式（创建引擎 + 算子预热推理）
    if (text.empty()) {
        OH_LOG_INFO(LOG_APP, "OHOS_load: prewarm engine starting in bg thread");
        std::string modelDirCopy = modelDir;
        auto prewarmStart = std::chrono::steady_clock::now();
        std::thread([modelDirCopy, prewarmStart]() {
            if (EnsureTtsEngine(modelDirCopy)) {
                // 算子预热：生成一个极短音频，让 ONNX Runtime 完成首次推理
                // 消除真实首次调用时的 500~2000ms 冷启动延迟
                OH_LOG_INFO(LOG_APP, "OHOS_load: TTS warmup inference start");
                g_ttsMutex.lock();
                auto warmStart = std::chrono::steady_clock::now();
                const SherpaOnnxGeneratedAudio* warmAudio =
                    SherpaOnnxOfflineTtsGenerate(g_ttsEngine, "。", 0, 1.0f);
                auto warmMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - warmStart).count();
                if (warmAudio) {
                    OH_LOG_INFO(LOG_APP, "OHOS_load: TTS warmup done in %{public}lld ms, audio=%{public}d samples",
                        (long long)warmMs, warmAudio->n);
                    SherpaOnnxDestroyOfflineTtsGeneratedAudio(warmAudio);
                } else {
                    OH_LOG_WARN(LOG_APP, "OHOS_load: TTS warmup returned null");
                }
                g_ttsMutex.unlock();
            }
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - prewarmStart).count();
            OH_LOG_INFO(LOG_APP, "OHOS_load: prewarm done in %{public}lld ms", (long long)ms);
        }).detach();
        napi_value result;
        napi_get_undefined(env, &result);
        return result;
    }

    double speed;
    napi_get_value_double(env, argv[2], &speed);
    auto* ctx = new TtsCallbackContext;
    ctx->isGenerating = new std::atomic<bool>(true);
    ctx->text = text;
    ctx->modelDir = modelDir;
    ctx->speed = (float)speed;
    OH_LOG_INFO(LOG_APP, "ohos_TTS ctx: text=%{public}s, modelDir=%{public}s, speed=%.1f",
                text.c_str(), modelDir.c_str(), speed);
    napi_value resource_name;
    napi_create_string_utf8(env, "ttsCallback", NAPI_AUTO_LENGTH, &resource_name);
    // 使用共享 TSFN（所有句子共用一个，保证回调顺序）
    if (!g_ttsSharedTsFunction) {
        napi_create_threadsafe_function(env, argv[3], nullptr, resource_name,
            0, 1, nullptr, nullptr, nullptr, CallJsCallback, &g_ttsSharedTsFunction);
    }
    ctx->tsfn = g_ttsSharedTsFunction;
    g_ttsActiveThreads.fetch_add(1);
    std::thread(BackgroundTtsThread, ctx).detach();
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value StopTts(napi_env env, napi_callback_info info) {
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// ============================================================
// SetPerfCallback
// ============================================================
struct PerfCallbackData {
    double currentRtf, avgRtf, maxRtf;
    int64_t lastLatencyMs, vadLatencyMs;
    bool needsDegradation;
};
static napi_threadsafe_function g_perfTsFunction = nullptr;
static napi_value SetPerfCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return nullptr;
    if (g_perfTsFunction) { napi_release_threadsafe_function(g_perfTsFunction, napi_tsfn_release); g_perfTsFunction = nullptr; }
    napi_value resource_name;
    napi_create_string_utf8(env, "perfCallback", NAPI_AUTO_LENGTH, &resource_name);
    napi_create_threadsafe_function(env, argv[0], nullptr, resource_name, 0, 1, nullptr, nullptr, nullptr,
        [](napi_env env, napi_value jsCallback, void* context, void* data) {
            if (!data) return;
            PerfCallbackData* pd = static_cast<PerfCallbackData*>(data);
            napi_value undefined, args[6];
            napi_get_undefined(env, &undefined);
            napi_create_double(env, pd->currentRtf, &args[0]);
            napi_create_double(env, pd->avgRtf, &args[1]);
            napi_create_double(env, pd->maxRtf, &args[2]);
            napi_create_int64(env, pd->lastLatencyMs, &args[3]);
            napi_create_int64(env, pd->vadLatencyMs, &args[4]);
            napi_get_boolean(env, pd->needsDegradation, &args[5]);
            napi_call_function(env, undefined, jsCallback, 6, args, nullptr);
            delete pd;
        }, &g_perfTsFunction);
    napi_value result; napi_get_undefined(env, &result); return result;
}

// ============================================================
// RawFile API
// ============================================================
static napi_value InitRawfileMgmt(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) { napi_value r; napi_get_boolean(env, false, &r); return r; }
    if (g_nativeResMgr) { OH_ResourceManager_ReleaseNativeResourceManager(g_nativeResMgr); g_nativeResMgr = nullptr; }
    napi_value resMgrVal;
    napi_get_named_property(env, argv[0], "resourceManager", &resMgrVal);
    g_nativeResMgr = OH_ResourceManager_InitNativeResourceManager(env, resMgrVal);
    napi_value r; napi_get_boolean(env, g_nativeResMgr != nullptr, &r); return r;
}
static napi_value ReadRawFileSync(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1 || !g_nativeResMgr) { napi_value r; napi_get_undefined(env, &r); return r; }
    size_t len; napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string rawPath(len, '\0'); napi_get_value_string_utf8(env, argv[0], &rawPath[0], len + 1, &len); rawPath.resize(len);
    RawFile* rf = OH_ResourceManager_OpenRawFile(g_nativeResMgr, rawPath.c_str());
    if (!rf) { napi_value r; napi_get_undefined(env, &r); return r; }
    long sz = OH_ResourceManager_GetRawFileSize(rf);
    if (sz <= 0) { OH_ResourceManager_CloseRawFile(rf); napi_value r; napi_get_undefined(env, &r); return r; }
    std::vector<char> buf(sz);
    int rs = OH_ResourceManager_ReadRawFile(rf, buf.data(), sz);
    OH_ResourceManager_CloseRawFile(rf);
    if (rs <= 0) { napi_value r; napi_get_undefined(env, &r); return r; }
    napi_value ab; void* data = nullptr;
    napi_create_arraybuffer(env, rs, &data, &ab);
    if (data) memcpy(data, buf.data(), rs);
    return ab;
}

// ============================================================
// 全双工音频状态机
// ============================================================
static napi_value StartFullDuplex(napi_env env, napi_callback_info info) {
    g_audioStateMachineConfig.interruptThreshold = 0.035f;
    g_audioStateMachineConfig.interruptMinFrames = 3;
    g_audioStateMachineConfig.debugLog = true;
    g_audioStateMachine.Init(g_audioStateMachineConfig);

    // 注册状态机 → ArkTS 动作回调
    g_audioStateMachine.SetOnActionRequest([](const std::string& action, const std::string& payload) {
        // 打断相关动作：直接 C++ 侧处理（无需 ArkTS 往返）
        if (action == "interrupted") {
            OH_LOG_INFO(LOG_APP, "ohos_FD interrupt action: clearing ASR stream");
            g_audioRingBuffer.Clear();
            if (g_asrStream && g_asrRecognizer) {
                SherpaOnnxOnlineStreamReset(g_asrRecognizer, g_asrStream);
            }
            g_wakeWordDetected.store(false);
        }
        // 转发到 ArkTS（通过 TSFN）
        if (g_duplexActionTsFunction) {
            auto* data = new DuplexActionData{action, payload};
            napi_call_threadsafe_function(g_duplexActionTsFunction, data, napi_tsfn_nonblocking);
        } else {
            OH_LOG_WARN(LOG_APP, "ohos_FD no duplex TSFN for action: %{public}s", action.c_str());
        }
    });
    g_audioStateMachine.SetOnStateChanged([](AudioState oldState, AudioState newState) {
        OH_LOG_INFO(LOG_APP, "ohos_FD state: %{public}s -> %{public}s",
                    AudioStateToString(oldState), AudioStateToString(newState));
    });

    g_audioStateMachine.Start();
    napi_value r; napi_get_undefined(env, &r); return r;
}
static napi_value StopFullDuplex(napi_env env, napi_callback_info info) {
    g_audioStateMachine.Stop();
    napi_value r; napi_get_undefined(env, &r); return r;
}
static napi_value GetFullDuplexState(napi_env env, napi_callback_info info) {
    napi_value r; napi_create_string_utf8(env, AudioStateToString(g_audioStateMachine.GetCurrentState()), NAPI_AUTO_LENGTH, &r); return r;
}
static napi_value NotifyDuplexEvent(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1 || !g_audioStateMachine.IsRunning()) return nullptr;
    size_t len; napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string evt(len, '\0'); napi_get_value_string_utf8(env, argv[0], &evt[0], len + 1, &len); evt.resize(len);

    std::string payload;
    if (argc >= 2) {
        size_t plen; napi_get_value_string_utf8(env, argv[1], nullptr, 0, &plen);
        payload.resize(plen, '\0');
        napi_get_value_string_utf8(env, argv[1], &payload[0], plen + 1, &plen);
        payload.resize(plen);
    }

    if (evt == "llm_start") g_audioStateMachine.OnLlmStart();
    else if (evt == "llm_complete") g_audioStateMachine.OnLlmComplete(payload);
    else if (evt == "tts_started") g_audioStateMachine.OnTtsStarted();
    else if (evt == "tts_complete") g_audioStateMachine.OnTtsComplete();
    else if (evt == "vad_start") g_audioStateMachine.OnVadSpeechStart();
    else if (evt == "vad_end") g_audioStateMachine.OnVadSpeechEnd();
    else if (evt == "interrupt") g_audioStateMachine.Interrupt();
    else OH_LOG_WARN(LOG_APP, "ohos_FD unknown event: %{public}s", evt.c_str());

    napi_value r; napi_get_undefined(env, &r); return r;
}
// ============================================================
// 性能监控 NAPI 接口
// ============================================================

// 启用/禁用开发者模式
static napi_value EnableDevMetrics(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return nullptr;

    bool enabled = false;
    napi_get_value_bool(env, argv[0], &enabled);

    if (enabled && !MetricsCollector::IsEnabled()) {
        MetricsCollector::SetEnabled(true);
    } else if (!enabled) {
        MetricsCollector::SaveToDisk();
        MetricsCollector::SetEnabled(false);
    }

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// 初始化存储路径（由 ArkTS 侧传入 filesDir）
static napi_value InitMetricsStorage(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return nullptr;

    size_t len; napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string path(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &path[0], len + 1, &len);
    path.resize(len);

    MetricsCollector::Init(path + "/perf/metrics.bin");
    // 如果已启用，自动开始采集
    if (MetricsCollector::IsEnabled()) {
        MetricsCollector::SetEnabled(true);
    }
    OH_LOG_INFO(LOG_APP, "ohos_Metrics storage initialized: %{public}s", path.c_str());

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// ArkTS 侧埋点入口：recordMetric(typeId, value)
static napi_value RecordMetric(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) return nullptr;

    int32_t typeId = 0;
    napi_get_value_int32(env, argv[0], &typeId);
    double value = 0.0;
    napi_get_value_double(env, argv[1], &value);

    MetricsCollector::Record(static_cast<MetricType>(typeId), value);
    return nullptr;
}

// 获取时序数据：getMetricTimeSeries(typeId, sinceMs) → ArrayBuffer
static napi_value GetMetricTimeSeries(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return nullptr;

    int32_t typeId = 0;
    napi_get_value_int32(env, argv[0], &typeId);
    int64_t sinceMs = 0;
    if (argc >= 2) napi_get_value_int64(env, argv[1], &sinceMs);

    auto points = MetricsCollector::GetTimeSeries(static_cast<MetricType>(typeId), sinceMs);
    size_t count = points.size();

    // 返回 { count, points: [{timestamp, value}] }
    napi_value result;
    napi_create_object(env, &result);

    napi_value jsCount;
    napi_create_uint32(env, count, &jsCount);
    napi_set_named_property(env, result, "count", jsCount);

    napi_value jsArray;
    napi_create_array_with_length(env, count, &jsArray);
    for (size_t i = 0; i < count; i++) {
        napi_value pt;
        napi_create_object(env, &pt);
        napi_value ts; napi_create_int64(env, points[i].timestamp, &ts);
        napi_value val; napi_create_double(env, points[i].value, &val);
        napi_set_named_property(env, pt, "t", ts);
        napi_set_named_property(env, pt, "v", val);
        napi_set_element(env, jsArray, i, pt);
    }
    napi_set_named_property(env, result, "points", jsArray);

    return result;
}

// 获取统计摘要：getMetricSummary(typeId) → { avg, max, min, latest, count, p50, p95, p99 }
static napi_value GetMetricSummary(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) return nullptr;

    int32_t typeId = 0;
    napi_get_value_int32(env, argv[0], &typeId);

    auto summary = MetricsCollector::GetSummary(static_cast<MetricType>(typeId));

    napi_value result;
    napi_create_object(env, &result);

    auto setDouble = [&](const char* name, double val) {
        napi_value v; napi_create_double(env, val, &v); napi_set_named_property(env, result, name, v);
    };
    auto setUint = [&](const char* name, uint32_t val) {
        napi_value v; napi_create_uint32(env, val, &v); napi_set_named_property(env, result, name, v);
    };

    setDouble("avg", summary.avg);
    setDouble("max", summary.max);
    setDouble("min", summary.min);
    setDouble("latest", summary.latest);
    setUint("count", summary.count);
    setDouble("p50", summary.p50);
    setDouble("p95", summary.p95);
    setDouble("p99", summary.p99);

    return result;
}

// 获取最新 N 个数据点：getLatestMetricPoints(typeId, count) → Array
static napi_value GetLatestMetricPoints(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) return nullptr;

    int32_t typeId = 0;
    napi_get_value_int32(env, argv[0], &typeId);
    int32_t count = 60;
    napi_get_value_int32(env, argv[1], &count);

    auto points = MetricsCollector::GetLatestPoints(static_cast<MetricType>(typeId), count);
    size_t n = points.size();

    napi_value jsArray;
    napi_create_array_with_length(env, n, &jsArray);
    for (size_t i = 0; i < n; i++) {
        napi_value pt;
        napi_create_object(env, &pt);
        napi_value ts; napi_create_int64(env, points[i].timestamp, &ts);
        napi_value val; napi_create_double(env, points[i].value, &val);
        napi_set_named_property(env, pt, "t", ts);
        napi_set_named_property(env, pt, "v", val);
        napi_set_element(env, jsArray, i, pt);
    }
    return jsArray;
}

// 获取最新值（简化版，直接返回 double，避免 ArkTS 对象访问问题）
static napi_value GetMetricLatest(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) { napi_value r; napi_create_double(env, 0, &r); return r; }
    int32_t typeId = 0;
    napi_get_value_int32(env, argv[0], &typeId);
    double val = MetricsCollector::GetLatest(static_cast<MetricType>(typeId));
    napi_value result;
    napi_create_double(env, val, &result);
    return result;
}

// 重置所有指标
static napi_value ResetMetrics(napi_env env, napi_callback_info info) {
    MetricsCollector::Reset();
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// 导出 JSON
static napi_value ExportMetricsJson(napi_env env, napi_callback_info info) {
    std::string json = MetricsCollector::ExportJson();
    napi_value result;
    napi_create_string_utf8(env, json.c_str(), NAPI_AUTO_LENGTH, &result);
    return result;
}

// 检查是否发生了用户打断（已弃用 — 状态机通过 Action 回调直接通知）
static napi_value CheckInterruption(napi_env env, napi_callback_info info) {
    napi_value result;
    napi_get_boolean(env, false, &result);
    return result;
}

// 重置 ASR 流（打断后调用，不清除 recognizer，仅清缓存）
static napi_value ResetAsrStream(napi_env env, napi_callback_info info) {
    OH_LOG_INFO(LOG_APP, "ohos_ASR ResetAsrStream: clearing ring buffer and resetting stream");
    g_audioRingBuffer.Clear();
    if (g_asrStream && g_asrRecognizer) {
        SherpaOnnxOnlineStreamReset(g_asrRecognizer, g_asrStream);
        OH_LOG_INFO(LOG_APP, "ohos_ASR stream reset OK");
    }
    g_wakeWordDetected.store(false);
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// ============================================================
// 本地 LLM 推理（Qwen2.5-0.5B + ONNX Runtime）
// ============================================================

// 全局本地 LLM 引擎实例
static std::unique_ptr<LocalLlmEngine> g_localLlmEngine;

// LLM 流式回调 TSFN
static napi_threadsafe_function g_llmTokenTsFunction = nullptr;
static napi_threadsafe_function g_llmCompleteTsFunction = nullptr;

// LLM Token 回调
struct LlmTokenData {
    std::string token;
};

// LLM 完成回调
struct LlmCompleteData {
    std::string text;
    bool success;
};

static void LlmTokenCallJs(napi_env env, napi_value jsCallback, void* context, void* data) {
    if (!data) return;
    LlmTokenData* d = static_cast<LlmTokenData*>(data);
    napi_value undefined, jsToken;
    napi_get_undefined(env, &undefined);
    napi_create_string_utf8(env, d->token.c_str(), NAPI_AUTO_LENGTH, &jsToken);
    napi_call_function(env, undefined, jsCallback, 1, &jsToken, nullptr);
    delete d;
}

static void LlmCompleteCallJs(napi_env env, napi_value jsCallback, void* context, void* data) {
    if (!data) return;
    LlmCompleteData* d = static_cast<LlmCompleteData*>(data);
    napi_value undefined, jsText, jsSuccess;
    napi_get_undefined(env, &undefined);
    napi_create_string_utf8(env, d->text.c_str(), NAPI_AUTO_LENGTH, &jsText);
    napi_get_boolean(env, d->success, &jsSuccess);
    napi_value argv[2] = { jsText, jsSuccess };
    napi_call_function(env, undefined, jsCallback, 2, argv, nullptr);
    delete d;
}

// NAPI: callLocalLlm(text, historyJson, onToken, onComplete)
// 流式调用本地 LLM，通过 Token 回调逐 token 返回
static napi_value CallLocalLlm(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) return nullptr;

    // 解析 text
    size_t textLen;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &textLen);
    std::string text(textLen, '\0');
    napi_get_value_string_utf8(env, argv[0], &text[0], textLen + 1, &textLen);
    text.resize(textLen);

    // 创建引擎（首次调用时初始化）
    if (!g_localLlmEngine) {
        g_localLlmEngine = std::make_unique<LocalLlmEngine>();
        LlmConfig cfg;
        // 模型文件路径 — 需复制到沙箱目录 models/llm/
        // 下载: huggingface.co/Qwen/Qwen2.5-0.5B-Instruct
        // 转换为 ONNX: optimum-cli export onnx --model Qwen/Qwen2.5-0.5B-Instruct --quantize int4
        cfg.modelPath = "/data/storage/el2/base/haps/entry/files/models/llm/model.int4.onnx";
        cfg.tokenizerPath = "/data/storage/el2/base/haps/entry/files/models/llm/tokenizer.json";
        cfg.maxTokens = 1024;
        cfg.temperature = 0.7f;
        if (!g_localLlmEngine->Init(cfg)) {
            OH_LOG_ERROR(LOG_APP, "ohos_LLM: Local engine init failed");
            napi_value result;
            napi_get_undefined(env, &result);
            return result;
        }
    }

    // 创建 TSFN 回调
    napi_value resource_name;
    napi_create_string_utf8(env, "llmTokenCallback", NAPI_AUTO_LENGTH, &resource_name);
    napi_create_threadsafe_function(env, argv[2], nullptr, resource_name,
        0, 1, nullptr, nullptr, nullptr, LlmTokenCallJs, &g_llmTokenTsFunction);
    
    napi_create_string_utf8(env, "llmCompleteCallback", NAPI_AUTO_LENGTH, &resource_name);
    napi_create_threadsafe_function(env, argv[3], nullptr, resource_name,
        0, 1, nullptr, nullptr, nullptr, LlmCompleteCallJs, &g_llmCompleteTsFunction);

    // 构建 messages
    std::vector<std::pair<std::string, std::string>> messages;
    messages.push_back({"user", text});

    // 流式调用
    g_localLlmEngine->CallStreaming(
        messages,
        [](const std::string& token) -> bool {
            if (g_llmTokenTsFunction) {
                auto* data = new LlmTokenData{token};
                napi_call_threadsafe_function(g_llmTokenTsFunction, data, napi_tsfn_nonblocking);
            }
            return true;
        },
        [](const std::string& fullText, bool success) {
            if (g_llmCompleteTsFunction) {
                auto* data = new LlmCompleteData{fullText, success};
                napi_call_threadsafe_function(g_llmCompleteTsFunction, data, napi_tsfn_blocking);
            }
            // 清理 TSFN
            if (g_llmTokenTsFunction) {
                napi_release_threadsafe_function(g_llmTokenTsFunction, napi_tsfn_release);
                g_llmTokenTsFunction = nullptr;
            }
            if (g_llmCompleteTsFunction) {
                napi_release_threadsafe_function(g_llmCompleteTsFunction, napi_tsfn_release);
                g_llmCompleteTsFunction = nullptr;
            }
        }
    );

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// NAPI: isLocalLlmAvailable() → boolean
// 检查本地 LLM 模型文件是否存在
static napi_value IsLocalLlmAvailable(napi_env env, napi_callback_info info) {
    // TODO: 检查模型文件是否存在
    napi_value result;
    napi_get_boolean(env, false, &result);
    return result;
}

// --- 模块注册 ---
EXTERN_C_START
extern "C" napi_value Init(napi_env env, napi_value exports) {
    OH_LOG_INFO(LOG_APP, "ohos_NAPI Init called.");
    napi_property_descriptor descs[] = {
        { "pingNative", 0, PingNative, 0, 0, 0, napi_default, 0 },
        { "startStreamingTtsWithSpeed", 0, StartStreamingTtsWithSpeed, 0, 0, 0, napi_default, 0 },
        { "stopTts", 0, StopTts, 0, 0, 0, napi_default, 0 },
        { "startAssistant", 0, StartAssistant, 0, 0, 0, napi_default, 0 },
        { "feedAudio", 0, FeedAudio, 0, 0, 0, napi_default, 0 },
        { "stopAssistant", 0, StopAssistant, 0, 0, 0, napi_default, 0 },
        { "setPerfCallback", 0, SetPerfCallback, 0, 0, 0, napi_default, 0 },
        { "initRawfileMgmt", 0, InitRawfileMgmt, 0, 0, 0, napi_default, 0 },
        { "readRawFileSync", 0, ReadRawFileSync, 0, 0, 0, napi_default, 0 },
        { "setDuplexCallbacks", 0, SetDuplexCallbacks, 0, 0, 0, napi_default, 0 },
        { "startFullDuplex", 0, StartFullDuplex, 0, 0, 0, napi_default, 0 },
        { "stopFullDuplex", 0, StopFullDuplex, 0, 0, 0, napi_default, 0 },
        { "getFullDuplexState", 0, GetFullDuplexState, 0, 0, 0, napi_default, 0 },
        { "notifyDuplexEvent", 0, NotifyDuplexEvent, 0, 0, 0, napi_default, 0 },
        // 打断检测
        { "checkInterruption", 0, CheckInterruption, 0, 0, 0, napi_default, 0 },
        { "resetAsrStream", 0, ResetAsrStream, 0, 0, 0, napi_default, 0 },
        // 本地 LLM
        { "callLocalLlm", 0, CallLocalLlm, 0, 0, 0, napi_default, 0 },
        { "isLocalLlmAvailable", 0, IsLocalLlmAvailable, 0, 0, 0, napi_default, 0 },
        // 性能监控
        { "enableDevMetrics", 0, EnableDevMetrics, 0, 0, 0, napi_default, 0 },
        { "initMetricsStorage", 0, InitMetricsStorage, 0, 0, 0, napi_default, 0 },
        { "recordMetric", 0, RecordMetric, 0, 0, 0, napi_default, 0 },
        { "getMetricTimeSeries", 0, GetMetricTimeSeries, 0, 0, 0, napi_default, 0 },
        { "getMetricSummary", 0, GetMetricSummary, 0, 0, 0, napi_default, 0 },
        { "getLatestMetricPoints", 0, GetLatestMetricPoints, 0, 0, 0, napi_default, 0 },
        { "getMetricLatest", 0, GetMetricLatest, 0, 0, 0, napi_default, 0 },
        { "resetMetrics", 0, ResetMetrics, 0, 0, 0, napi_default, 0 },
        { "exportMetricsJson", 0, ExportMetricsJson, 0, 0, 0, napi_default, 0 },
    };
    napi_define_properties(env, exports, sizeof(descs) / sizeof(descs[0]), descs);
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "native_lib",
    .nm_priv = ((void*)0),
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterNativeModule(void) {
    OH_LOG_INFO(LOG_APP, "ohos_RegisterNativeModule called.");
    napi_module_register(&demoModule);
}