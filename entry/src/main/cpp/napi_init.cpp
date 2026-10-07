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
#include <memory>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>
#include "third_party/llama.cpp/vendor/nlohmann/json.hpp"

// 引入新版 sherpa-onnx C API
#include "sherpa-onnx/c-api.h"
#include "RingBuffer.h"
#include <map>
#include <unordered_map>
#include <unordered_set>

// 无锁环形缓冲区：音频采集（生产者）→ ASR 推理（消费者）
// 容量 320000 float ≈ 20秒 @16kHz，避免 mutex 竞争
static RingBuffer<float> g_audioRingBuffer(320000);

// RawFile API - 从 HAP 读取模型文件
#include "rawfile/raw_file_manager.h"
#include "rawfile/raw_file.h"
static NativeResourceManager* g_nativeResMgr = nullptr;

// 全双工音频状态机
#include "audio/AudioStateMachine.h"
#include "audio/PipelineTypes.h"
#include "llm/LocalLlmEngine.h"
#include "llm/GgufLlmEngine.h"
#include "llm/RemoteLlmEngine.h"
static AudioStateMachine g_audioStateMachine;
static AudioStateMachineConfig g_audioStateMachineConfig;

// 声学回声消除 (AEC) - NLMS 自适应滤波器
// 使用 unique_ptr + 显式初始化，避免静态初始化顺序问题
#include "audio/Aec.h"
#include <memory>
static std::unique_ptr<AcousticEchoCanceller> g_aec;

// 噪声抑制 (RNNoise)
#include "audio/NoiseSuppressor.h"
#include "utils/CancelScope.h"
static std::unique_ptr<NoiseSuppressor> g_noiseSuppressor;

// LLM 引擎全局实例（声明在 Lambda 之前，确保被 Capture）
static std::unique_ptr<LlmEngine> g_localLlmEngine;
// Protects ownership/lifecycle of the process-wide local LLM instance.
static std::mutex g_localLlmMutex;

// 性能监控采集器
#include "metrics/MetricsCollector.h"
#include "metrics/BenchmarkEngine.h"

// --- 性能监控工具 ---
class PerformanceProfiler {
public:
    static void Start(const std::string& tag) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_startTimes[tag] = std::chrono::steady_clock::now();
    }

    static void End(const std::string& tag) {
        std::lock_guard<std::mutex> lock(m_mutex);
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
    static std::mutex m_mutex;
};

std::map<std::string, std::chrono::steady_clock::time_point> PerformanceProfiler::m_startTimes;
std::mutex PerformanceProfiler::m_mutex;

// FeedAudio 内部状态（替代静态局部变量，支持 Reset）
struct FeedAudioState {
    float prevInput = 0.0f;
    float prevOutput = 0.0f;
    std::chrono::steady_clock::time_point ttsEndTime;
    bool prevTtsPlaying = false;
    int aecLogCounter = 0;
    int feedCounter = 0;

    // VAD 累加器（之前是 static 局部变量，多轮残留问题）
    std::vector<float> vadAccumulator;
    std::vector<float> bargeInVadAccumulator;
    std::vector<float> bargeInPreRoll;
    std::vector<float> bargeInCandidateAudio;
    float bargeInNoiseFloor = 0.003f;
    int bargeInVoiceFrames = 0;
    int bargeInWindowFrames = 0;
    int bargeInMissFrames = 0;
    bool bargeInCandidateActive = false;
    bool vadLoggedPath = false;
    std::string lastAsrPartial;  // 上次 ASR 中间结果，用于去重
};
static FeedAudioState g_feedAudioState;
// Guards every mutation/read of FeedAudioState that can overlap a duplex
// notification. In particular, tts_stopped/tts_complete may arrive from a
// different ArkTS worker while FeedAudio is erasing the VAD accumulator.
static std::mutex g_bargeInMutex;
static std::atomic<bool> g_bargeInHandoffPending{false};

// Barge-in buffer capacity tuned in ResetFeedAudioState / StartAssistant.

static void ResetFeedAudioState() {
    std::lock_guard<std::mutex> stateLock(g_bargeInMutex);
    g_feedAudioState.prevInput = 0.0f;
    g_feedAudioState.prevOutput = 0.0f;
    g_feedAudioState.ttsEndTime = std::chrono::steady_clock::time_point();
    g_feedAudioState.prevTtsPlaying = false;
    g_feedAudioState.aecLogCounter = 0;
    g_feedAudioState.feedCounter = 0;
    g_feedAudioState.vadAccumulator.clear();
    g_feedAudioState.vadAccumulator.shrink_to_fit();  // 释放内存
    g_feedAudioState.bargeInVadAccumulator.clear();
    g_feedAudioState.bargeInVadAccumulator.shrink_to_fit();
    g_feedAudioState.bargeInPreRoll.clear();
    g_feedAudioState.bargeInPreRoll.reserve(3200);    // 预分配防 realloc
    g_feedAudioState.bargeInCandidateAudio.clear();
    g_feedAudioState.bargeInCandidateAudio.reserve(160000);
    g_feedAudioState.bargeInNoiseFloor = 0.003f;
    g_feedAudioState.bargeInVoiceFrames = 0;
    g_feedAudioState.bargeInWindowFrames = 0;
    g_feedAudioState.bargeInMissFrames = 0;
    g_feedAudioState.bargeInCandidateActive = false;
    g_bargeInHandoffPending.store(false);
    g_feedAudioState.vadLoggedPath = false;
    g_feedAudioState.lastAsrPartial.clear();
    g_audioRingBuffer.Clear();
}

// ============================================================
// ASR + VAD 实现
// ============================================================

// --- ASR 全局状态 ---
static std::mutex g_asrMutex;
static std::atomic<bool> g_isAsrRunning(false);
// Online stream 只能由解码线程操作。其他线程只投递 reset 请求，避免
// DecodeOnlineStream 与 OnlineStreamReset 并发进入 sherpa-onnx。
static std::atomic<bool> g_asrResetRequested(false);
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
static const SherpaOnnxVoiceActivityDetector* g_bargeInVad = nullptr;
static std::atomic<bool> g_asrPrewarmed(false);
static std::atomic<bool> g_vadLoaded(false);

static const SherpaOnnxOfflineTts* g_ttsEngine = nullptr;
static std::mutex g_ttsMutex;
static std::string g_ttsModelDir;
static std::atomic<int32_t> g_ttsNumThreads{4};
static std::mutex g_ttsFlowMutex;
static std::condition_variable g_ttsFlowCv;
static uint64_t g_ttsFlowGeneration = 0;
static int64_t g_ttsOutstandingSamples = 0;
// Prevent lifecycle timers from destroying the engine during a diagnostic run.
static std::atomic<int32_t> g_ttsLifecycleLeaseCount{0};

// keyword spotter 已移除，使用 VAD 驱动的全双工模式


// --- 音频缓冲区（JS 线程写入，ASR 线程读取）---
static std::mutex g_audioBufMutex;
static std::vector<float> g_audioBuffer;

// ASR 工作线程
static std::thread g_asrThread;

static std::string NormalizeShortAsrText(std::string text) {
    static const char* kIgnoredTokens[] = {
        " ", "\t", "\r", "\n", ",", ".", "!", "?", "~",
        "，", "。", "！", "？", "、", "～"
    };
    for (const char* token : kIgnoredTokens) {
        const size_t tokenLen = std::strlen(token);
        size_t pos = 0;
        while ((pos = text.find(token, pos)) != std::string::npos) {
            text.erase(pos, tokenLen);
        }
    }
    return text;
}

static bool ShouldDiscardAsrFinal(const std::string& text) {
    const std::string normalized = NormalizeShortAsrText(text);
    if (normalized.empty()) return true;
    static const std::unordered_set<std::string> kFillers = {
        "嗯", "嗯嗯", "啊", "啊啊", "呃", "呃呃", "额", "额额",
        "哦", "噢", "唉", "哎", "诶", "欸", "哈", "哈哈", "呵",
        "哼", "唔"
    };
    return kFillers.find(normalized) != kFillers.end();
}

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
    auto lastPartialAt = std::chrono::steady_clock::now();

    while (g_isAsrRunning.load()) {
        if (g_asrResetRequested.exchange(false)) {
            if (g_asrRecognizer && g_asrStream) {
                SherpaOnnxOnlineStreamReset(g_asrRecognizer, g_asrStream);
            }
            if (g_vad) {
                SherpaOnnxVoiceActivityDetectorReset(g_vad);
            }
            {
                std::lock_guard<std::mutex> stateLock(g_bargeInMutex);
                g_feedAudioState.vadAccumulator.clear();
            }
            lastText.clear();
            g_audioStateMachine.OnAsrInterim("");
            MetricsCollector::Record(MetricType::AsrDecoderReset, 1.0);
            OH_LOG_INFO(LOG_APP, "ohos_ASR decoder-owned stream reset completed");
        }

        std::vector<float> localBuf;
        // 80 ms 一批，降低在线字幕延迟，也减少 VAD 定稿时误带入下一句话的音频。
        constexpr size_t kStreamingBatchSamples = 1280;
        localBuf.reserve(kStreamingBatchSamples);
        size_t samplesRead = g_audioRingBuffer.ReadToVector(localBuf, kStreamingBatchSamples);

        if (samplesRead == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // ======== VAD/ASR 处理 ========
        if (localBuf.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // 音频实时送入在线 ASR，VAD 只负责确定句尾。
        // 不能等 VAD 收齐完整语音段后再喂 ASR，否则中间结果只会在说完后集中出现。
        if (g_vad) {
            if (!g_feedAudioState.vadLoggedPath) {
                OH_LOG_INFO(LOG_APP, "ohos_ASR PATH: live online ASR + VAD endpoint");
                g_feedAudioState.vadLoggedPath = true;
            }

            SherpaOnnxOnlineStreamAcceptWaveform(g_asrStream, 16000,
                localBuf.data(), static_cast<int32_t>(localBuf.size()));

            const double audioDurationMs = static_cast<double>(localBuf.size()) / 16000.0 * 1000.0;
            const auto decodeStart = std::chrono::steady_clock::now();
            PerformanceProfiler::Start("ASR Live Decode");
            while (SherpaOnnxIsOnlineStreamReady(g_asrRecognizer, g_asrStream)) {
                SherpaOnnxDecodeOnlineStream(g_asrRecognizer, g_asrStream);
            }
            PerformanceProfiler::End("ASR Live Decode");
            const auto decodeEnd = std::chrono::steady_clock::now();
            const double decodeMs =
                std::chrono::duration_cast<std::chrono::microseconds>(decodeEnd - decodeStart).count() / 1000.0;
            MetricsCollector::RecordAsrRtf(audioDurationMs > 0.0 ? decodeMs / audioDurationMs : 0.0);
            MetricsCollector::RecordAsrDecodeLatency(decodeMs);
            MetricsCollector::RecordRingBufferFillRate(
                static_cast<double>(g_audioRingBuffer.Size()) /
                static_cast<double>(g_audioRingBuffer.Capacity()));

            const SherpaOnnxOnlineRecognizerResult* liveResult =
                SherpaOnnxGetOnlineStreamResult(g_asrRecognizer, g_asrStream);
            if (liveResult && liveResult->text && strlen(liveResult->text) > 0) {
                std::string currentText(liveResult->text);
                if (currentText != lastText) {
                    lastText = currentText;
                    lastPartialAt = std::chrono::steady_clock::now();
                    if (!ShouldDiscardAsrFinal(currentText)) {
                        OH_LOG_INFO(LOG_APP, "ohos_ASR live partial: %{public}s", currentText.c_str());
                        g_audioStateMachine.OnAsrInterim(currentText);
                    }
                }
            }
            if (liveResult) SherpaOnnxDestroyOnlineRecognizerResult(liveResult);

            // 累积音频到至少 2560 samples (160ms) 再批量喂给 VAD
            // 避免零星 40ms 音频导致 VAD 将音节间短停顿误判为句子结束
            // Move a complete batch out under the shared state lock. Duplex
            // callbacks are then free to reset the accumulator without
            // invalidating the pointer passed to sherpa-onnx.
            std::vector<float> vadBatch;
            {
                std::lock_guard<std::mutex> stateLock(g_bargeInMutex);
                g_feedAudioState.vadAccumulator.insert(
                    g_feedAudioState.vadAccumulator.end(), localBuf.begin(), localBuf.end());
                if (g_feedAudioState.vadAccumulator.size() >= 2560) {
                    vadBatch.swap(g_feedAudioState.vadAccumulator);
                }
            }

            if (!vadBatch.empty()) {
                OH_LOG_INFO(LOG_APP, "ohos_ASR VAD feed: accumulated %{public}zu samples",
                    vadBatch.size());

                auto vadStart = std::chrono::high_resolution_clock::now();
                SherpaOnnxVoiceActivityDetectorAcceptWaveform(g_vad,
                    vadBatch.data(), static_cast<int32_t>(vadBatch.size()));
                auto vadEnd = std::chrono::high_resolution_clock::now();
                auto vadNs = std::chrono::duration_cast<std::chrono::nanoseconds>(vadEnd - vadStart).count();
                MetricsCollector::RecordVadLatency(static_cast<double>(vadNs) / 1000000.0);
            }

            // VAD 检测到句尾后，只定稿当前在线 stream；语音段本身不能重复喂给 ASR。
            while (!SherpaOnnxVoiceActivityDetectorEmpty(g_vad) && g_isAsrRunning.load()) {
                const SherpaOnnxSpeechSegment* seg = SherpaOnnxVoiceActivityDetectorFront(g_vad);
                if (seg && seg->samples && seg->n > 0) {
                    // VAD 已经观察到配置的尾部静音，通知在线识别器排空右上下文。
                    SherpaOnnxOnlineStreamInputFinished(g_asrStream);
                    while (SherpaOnnxIsOnlineStreamReady(g_asrRecognizer, g_asrStream)) {
                        SherpaOnnxDecodeOnlineStream(g_asrRecognizer, g_asrStream);
                    }
                    double segAudioDurationMs = (double)seg->n / 16000.0 * 1000.0;
                    MetricsCollector::Record(MetricType::VadSpeechDurationMs, segAudioDurationMs);
                    MetricsCollector::RecordRingBufferFillRate(
                        (double)g_audioRingBuffer.Size() / (double)g_audioRingBuffer.Capacity());

                    // flush 后正常应有完整结果；若底层返回空对象，回退到最近一次实时结果。
                    const SherpaOnnxOnlineRecognizerResult* r =
                        SherpaOnnxGetOnlineStreamResult(g_asrRecognizer, g_asrStream);
                    std::string currentText =
                        (r && r->text && strlen(r->text) > 0) ? std::string(r->text) : lastText;
                    if (!currentText.empty() && !ShouldDiscardAsrFinal(currentText)) {
                        OH_LOG_INFO(LOG_APP, "ohos_ASR VAD result: seg=%{public}d samples(%.0fms) → \"%{public}s\"",
                            seg->n, segAudioDurationMs, currentText.c_str());
                OH_LOG_INFO(LOG_APP, "OHOS_load: VAD seg=%.0fms text=\"%{public}s\"",
                    segAudioDurationMs, currentText.c_str());
                        // 通过状态机处理 ASR 最终结果
                        g_audioStateMachine.OnAsrFinal(currentText);
                    } else if (!currentText.empty()) {
                        MetricsCollector::Record(MetricType::AsrFillerDiscard, 1.0);
                        OH_LOG_INFO(LOG_APP,
                            "ohos_ASR discarded filler/noise final: \"%{public}s\" duration=%.0fms",
                            currentText.c_str(), segAudioDurationMs);
                        g_audioStateMachine.OnAsrInterim("");
                    } else {
                        OH_LOG_INFO(LOG_APP, "ohos_ASR segment result: EMPTY (r=%p, text=%p)", r, r ? r->text : nullptr);
                        g_audioStateMachine.OnAsrInterim("");
                    }
                    if (r) SherpaOnnxDestroyOnlineRecognizerResult(r);

                    // 重置 stream，为下一个语音段做准备
                    SherpaOnnxOnlineStreamReset(g_asrRecognizer, g_asrStream);
                    lastText.clear();
                    // 不清理 RingBuffer — VAD 已经消费了音频，剩余音频属于下一句
                    OH_LOG_INFO(LOG_APP, "ohos_ASR VAD segment done (ring not cleared)");
                }
                SherpaOnnxDestroySpeechSegment(seg);
                SherpaOnnxVoiceActivityDetectorPop(g_vad);
            }

            // ASR 可能从一次轻微碰撞/气流中产生 partial，但该声音没有达到
            // VAD 的有效语音段条件，因此永远不会形成 final。若 VAD 已连续
            // 900 ms 判定无人声，丢弃这个孤儿 partial，避免污染下一句话。
            const bool vadSpeechDetected =
                SherpaOnnxVoiceActivityDetectorDetected(g_vad) != 0;
            const auto partialIdleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - lastPartialAt).count();
            if (!lastText.empty() && !vadSpeechDetected && partialIdleMs >= 900) {
                OH_LOG_INFO(LOG_APP,
                    "ohos_ASR orphan partial reset after %{public}lldms: \"%{public}s\"",
                    static_cast<long long>(partialIdleMs), lastText.c_str());
                MetricsCollector::Record(MetricType::AsrOrphanPartialReset, 1.0);
                SherpaOnnxOnlineStreamReset(g_asrRecognizer, g_asrStream);
                SherpaOnnxVoiceActivityDetectorReset(g_vad);
                {
                    std::lock_guard<std::mutex> stateLock(g_bargeInMutex);
                    g_feedAudioState.vadAccumulator.clear();
                }
                lastText.clear();
                g_audioStateMachine.OnAsrInterim("");
            }
        } else {
            if (!g_feedAudioState.vadLoggedPath) { OH_LOG_INFO(LOG_APP, "ohos_ASR PATH: Non-VAD path (may have 叠字)"); g_feedAudioState.vadLoggedPath = true; }
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
                    lastPartialAt = std::chrono::steady_clock::now();
                    if (!ShouldDiscardAsrFinal(currentText)) {
                        OH_LOG_INFO(LOG_APP, "ohos_ASR partial: %{public}s", currentText.c_str());
                        // 中间结果 → 状态机转发到 ArkTS 显示
                        g_audioStateMachine.OnAsrInterim(currentText);
                    }
                }
            }
            if (r) SherpaOnnxDestroyOnlineRecognizerResult(r);

            // 端点检测
            if (SherpaOnnxOnlineStreamIsEndpoint(g_asrRecognizer, g_asrStream)) {
                OH_LOG_INFO(LOG_APP, "ohos_ASR endpoint TRIGGERED, lastText='%{public}s' len=%{public}zu",
                    lastText.c_str(), lastText.size());
                if (!lastText.empty() && !ShouldDiscardAsrFinal(lastText)) {
                    OH_LOG_INFO(LOG_APP, "ohos_ASR endpoint: %{public}s", lastText.c_str());
                    g_audioStateMachine.OnAsrFinal(lastText);
                    lastText = "";
                    // 清除环形缓冲区残留，防止同一段语音被重复识别
                    g_audioRingBuffer.Clear();
                    OH_LOG_INFO(LOG_APP, "ohos_ASR cleared ring buffer after endpoint");
                } else if (!lastText.empty()) {
                    MetricsCollector::Record(MetricType::AsrFillerDiscard, 1.0);
                    OH_LOG_INFO(LOG_APP, "ohos_ASR discarded filler/noise endpoint: \"%{public}s\"",
                        lastText.c_str());
                    lastText.clear();
                    g_audioStateMachine.OnAsrInterim("");
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
    // 4 路 beam 在移动端显著降低在线解码负担；保留 modified beam search 的稳定性。
    recognizerConfig.max_active_paths = 4;
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

    // --- KWS 创建移到外部（每次真实启动时创建，不在预热时运行）---

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
        if (g_vad) {
            OH_LOG_INFO(LOG_APP, "ohos_ASR VAD created OK");
        } else {
            OH_LOG_WARN(LOG_APP, "ohos_ASR VAD creation failed, running without VAD");
        }

        // 独立的低延迟打断 VAD：比 ASR 分段 VAD 更严格、更短，避免环境声触发打断。
        SherpaOnnxVadModelConfig bargeInVadConfig = vadConfig;
        bargeInVadConfig.silero_vad.threshold = 0.42f;
        bargeInVadConfig.silero_vad.min_speech_duration = 0.08f;
        bargeInVadConfig.silero_vad.min_silence_duration = 0.15f;
        g_bargeInVad = SherpaOnnxCreateVoiceActivityDetector(&bargeInVadConfig, 5.0f);
        g_vadLoaded.store(g_vad != nullptr || g_bargeInVad != nullptr);
        OH_LOG_INFO(LOG_APP, "ohos_BARGE_VAD: %{public}s", g_bargeInVad ? "created" : "unavailable");
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
            SherpaOnnxVadModelConfig bargeInVadConfig = vadConfig;
            bargeInVadConfig.silero_vad.threshold = 0.42f;
            bargeInVadConfig.silero_vad.min_speech_duration = 0.08f;
            bargeInVadConfig.silero_vad.min_silence_duration = 0.15f;
            g_bargeInVad = SherpaOnnxCreateVoiceActivityDetector(&bargeInVadConfig, 5.0f);
            g_vadLoaded.store(g_vad != nullptr || g_bargeInVad != nullptr);
            OH_LOG_INFO(LOG_APP, "ohos_ASR VAD recreated (fast recovery path)");
            OH_LOG_INFO(LOG_APP, "ohos_BARGE_VAD: recreated=%{public}d", g_bargeInVad != nullptr);
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

    // 清空环形缓冲区，启动解码线程
    // 先确保旧线程已退出，再启动新线程
    if (g_asrThread.joinable()) {
        g_isAsrRunning.store(false);
        g_asrThread.join();
    }
    // The old decoder must be gone before clearing/shrinking its accumulator.
    ResetFeedAudioState();
    g_asrResetRequested.store(true);
    g_isAsrRunning.store(true);
    g_asrThread = std::thread(BackgroundAsrThread);
    OH_LOG_INFO(LOG_APP, "ohos_ASR Assistant started, background thread launched");
    MetricsCollector::RecordProcessMemorySnapshot();
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

    // Audio capture callbacks and duplex notifications are delivered by
    // different workers. Serialize the stateful DC/VAD/barge-in pipeline so a
    // notification cannot clear a vector while this function is using it.
    std::lock_guard<std::mutex> stateLock(g_bargeInMutex);
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
    auto pipelineStart = std::chrono::high_resolution_clock::now();

    std::vector<float> floatSamples(sampleCount);
    // DC 阻挡高通滤波器：一阶 IIR，截止频率 ~40Hz @16kHz
    // y[n] = x[n] - x[n-1] + 0.95 * y[n-1]
    auto dcStart = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < sampleCount; i++) {
        float x = (float)pcm[i] / 32768.0f;
        float y = x - g_feedAudioState.prevInput + 0.95f * g_feedAudioState.prevOutput;
        g_feedAudioState.prevInput = x;
        g_feedAudioState.prevOutput = y;
        floatSamples[i] = y;
    }
    auto dcEnd = std::chrono::high_resolution_clock::now();
    MetricsCollector::RecordDcFilterLatency(
        std::chrono::duration_cast<std::chrono::microseconds>(dcEnd - dcStart).count());

    // The communication capturer has already applied platform AEC. Preserve a
    // copy before RNNoise so quiet near-end speech is not suppressed twice.
    std::vector<float> bargeInSamples = floatSamples;

    // === 1.5 噪声抑制（DC 之后、AEC 之前，降低背景噪声）===
    // 使用 RNNoise 做实时噪声抑制，不影响 AEC 参考信号
    // 首次调用时自动初始化（lazy init），如果 librnnoise.so 不可用则静默降级
    if (!g_noiseSuppressor) {
        g_noiseSuppressor = std::make_unique<NoiseSuppressor>();
        if (!g_noiseSuppressor->Init()) {
            OH_LOG_INFO(LOG_APP, "ohos_NS: RNNoise not available, noise suppression disabled");
        }
    }
    if (g_noiseSuppressor && g_noiseSuppressor->IsInitialized()) {
        auto nsStart = std::chrono::high_resolution_clock::now();
        g_noiseSuppressor->Process(floatSamples.data(), sampleCount);
        auto nsEnd = std::chrono::high_resolution_clock::now();
        MetricsCollector::RecordNoiseSuppressLatency(
            std::chrono::duration_cast<std::chrono::microseconds>(nsEnd - nsStart).count());
    }

    // === 2. AEC 回声消除（先于状态机，防止回声误判为打断）===
    bool ttsPlaying = g_audioStateMachine.IsTtsPlaying();
    // SOURCE_TYPE_VOICE_COMMUNICATION supplies the platform-AEC microphone
    // signal. Running the custom NLMS AEC here as well can erase near-end voice.
    constexpr size_t kBargeInPreRollSamples = 3200; // 200 ms @ 16 kHz
    constexpr size_t kBargeInMaxSamples = 160000;   // 10 s safety cap
    if (ttsPlaying) {
        g_feedAudioState.bargeInPreRoll.insert(g_feedAudioState.bargeInPreRoll.end(),
            bargeInSamples.begin(), bargeInSamples.end());
        if (g_feedAudioState.bargeInPreRoll.size() > kBargeInPreRollSamples) {
            g_feedAudioState.bargeInPreRoll.erase(g_feedAudioState.bargeInPreRoll.begin(),
                g_feedAudioState.bargeInPreRoll.end() - kBargeInPreRollSamples);
        }
        if (g_feedAudioState.bargeInCandidateActive) {
            g_feedAudioState.bargeInCandidateAudio.insert(
                g_feedAudioState.bargeInCandidateAudio.end(), bargeInSamples.begin(), bargeInSamples.end());
            if (g_feedAudioState.bargeInCandidateAudio.size() > kBargeInMaxSamples) {
                const size_t excess = g_feedAudioState.bargeInCandidateAudio.size() - kBargeInMaxSamples;
                g_feedAudioState.bargeInCandidateAudio.erase(
                    g_feedAudioState.bargeInCandidateAudio.begin(),
                    g_feedAudioState.bargeInCandidateAudio.begin() + excess);
            }
        }
    } else if (g_bargeInHandoffPending.load()) {
        if (g_bargeInHandoffPending.load()) {
            g_feedAudioState.bargeInCandidateAudio.insert(
                g_feedAudioState.bargeInCandidateAudio.end(), bargeInSamples.begin(), bargeInSamples.end());
        }
    }

    // 记录管线总耗时
    auto pipelineEnd = std::chrono::high_resolution_clock::now();
    MetricsCollector::RecordAudioPipelineTotal(
        std::chrono::duration_cast<std::chrono::microseconds>(pipelineEnd - pipelineStart).count() / 1000.0);

    // === 3. TTS 播放结束后的静默保护期（防止回声残留被 ASR 捕获）===
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

    // === 4. Barge-in VAD：AEC 后的人声判定 + 自适应能量门控 ===
    if (ttsPlaying && g_bargeInVad) {
        g_feedAudioState.bargeInVadAccumulator.insert(
            g_feedAudioState.bargeInVadAccumulator.end(), bargeInSamples.begin(), bargeInSamples.end());
        while (g_feedAudioState.bargeInVadAccumulator.size() >= 512 &&
               g_audioStateMachine.IsTtsPlaying()) {
            const float* frame = g_feedAudioState.bargeInVadAccumulator.data();
            double sumSq = 0.0;
            for (int i = 0; i < 512; ++i) sumSq += frame[i] * frame[i];
            const float rms = static_cast<float>(std::sqrt(sumSq / 512.0));
            SherpaOnnxVoiceActivityDetectorAcceptWaveform(g_bargeInVad, frame, 512);
            const bool speech = SherpaOnnxVoiceActivityDetectorDetected(g_bargeInVad) != 0;
            if (!speech) {
                g_feedAudioState.bargeInNoiseFloor = 0.98f * g_feedAudioState.bargeInNoiseFloor + 0.02f * rms;
            }
            const float energyGate = std::max(0.0035f, g_feedAudioState.bargeInNoiseFloor * 1.3f);
            const bool nearEndEvidence = speech && rms >= energyGate;
            const bool candidateActive =
                g_audioStateMachine.GetCurrentState() == AudioState::BARGE_IN_CANDIDATE;
            if (nearEndEvidence) {
                ++g_feedAudioState.bargeInVoiceFrames;
                ++g_feedAudioState.bargeInWindowFrames;
                g_feedAudioState.bargeInMissFrames = 0;
                if (!candidateActive) {
                    if (!g_feedAudioState.bargeInCandidateActive) {
                        g_feedAudioState.bargeInCandidateActive = true;
                        g_feedAudioState.bargeInCandidateAudio = g_feedAudioState.bargeInPreRoll;
                    }
                    g_feedAudioState.bargeInVoiceFrames = 1;
                    g_feedAudioState.bargeInWindowFrames = 1;
                    g_audioStateMachine.OnVadSpeechStart();
                    OH_LOG_INFO(LOG_APP,
                        "ohos_BARGE_VAD: candidate rms=%.4f gate=%.4f floor=%.4f",
                        rms, energyGate, g_feedAudioState.bargeInNoiseFloor);
                }
                // Human speech is not frame-perfect: tolerate short consonants and
                // syllable gaps. Three positive 32 ms frames are enough once the
                // independent VAD and adaptive energy gate have both fired.
                const int confirmEvidenceFrames = 3;
                if (g_feedAudioState.bargeInVoiceFrames >= confirmEvidenceFrames) {
                    OH_LOG_INFO(LOG_APP,
                        "ohos_BARGE_VAD: confirmed evidence=%{public}d/%{public}d rms=%.4f floor=%.4f",
                        g_feedAudioState.bargeInVoiceFrames, g_feedAudioState.bargeInWindowFrames,
                        rms, g_feedAudioState.bargeInNoiseFloor);
                    if (!g_bargeInHandoffPending.exchange(true)) {
                        if (!g_audioStateMachine.ConfirmBargeIn()) {
                            g_bargeInHandoffPending.store(false);
                        }
                    }
                    g_feedAudioState.bargeInVoiceFrames = 0;
                    g_feedAudioState.bargeInWindowFrames = 0;
                    g_feedAudioState.bargeInMissFrames = 0;
                }
            } else {
                if (candidateActive) {
                    ++g_feedAudioState.bargeInWindowFrames;
                    ++g_feedAudioState.bargeInMissFrames;
                }
                // Allow up to 160 ms of weak/unvoiced audio. Cancel only after a
                // real gap, or when the evidence window expires without enough voice.
                if (candidateActive && (g_feedAudioState.bargeInMissFrames >= 6 ||
                    g_feedAudioState.bargeInWindowFrames >= 24)) {
                    OH_LOG_INFO(LOG_APP,
                        "ohos_BARGE_VAD: cancelled evidence=%{public}d/%{public}d misses=%{public}d",
                        g_feedAudioState.bargeInVoiceFrames, g_feedAudioState.bargeInWindowFrames,
                        g_feedAudioState.bargeInMissFrames);
                    g_audioStateMachine.CancelBargeIn();
                    g_feedAudioState.bargeInCandidateActive = false;
                    g_feedAudioState.bargeInCandidateAudio.clear();
                    g_feedAudioState.bargeInVoiceFrames = 0;
                    g_feedAudioState.bargeInWindowFrames = 0;
                    g_feedAudioState.bargeInMissFrames = 0;
                }
            }
            g_feedAudioState.bargeInVadAccumulator.erase(
                g_feedAudioState.bargeInVadAccumulator.begin(),
                g_feedAudioState.bargeInVadAccumulator.begin() + 512);
        }
    } else {
        g_feedAudioState.bargeInVadAccumulator.clear();
        g_feedAudioState.bargeInVoiceFrames = 0;
        g_feedAudioState.bargeInWindowFrames = 0;
        g_feedAudioState.bargeInMissFrames = 0;
    }

    // === 5. 兼容保留 RMS 兜底；VAD 路径优先过滤非人声 ===
    // 多道保障：
    //   - ttsPlaying=false → 正常处理
    //   - ttsPlaying=true + AEC 处理过 → 回声残留 < 0.01，不会误触发
    //   - ttsPlaying=true + 用户大声说话 → RMS > 0.02 → 打断
    if (g_audioStateMachine.IsRunning() && !ttsPlaying &&
        !g_bargeInHandoffPending.load() && !g_bargeInVad) {
        g_audioStateMachine.FeedAudio(floatSamples.data(), floatSamples.size());
    }

    // === 5. 喂给 ASR（AEC 收敛 + 非静默保护期）===
    if (!ttsPlaying && !g_bargeInHandoffPending.load() && !inMutePeriod) {
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

    // 重置 AEC 状态 + 噪声抑制
    if (g_aec) {
        g_aec->Reset();
        g_aec.reset();
    }
    if (g_noiseSuppressor) {
        g_noiseSuppressor->Reset();
        g_noiseSuppressor.reset();
    }

    {
        std::lock_guard<std::mutex> lock(g_ttsMutex);
        if (g_ttsEngine) {
            SherpaOnnxDestroyOfflineTts(g_ttsEngine);
            g_ttsEngine = nullptr;
            g_ttsModelDir.clear();
            OH_LOG_INFO(LOG_APP, "ohos_TTS_ENGINE: released");
        }
    }

    // ASR 结果通过状态机 Action 回调传递，无需独立 TSFN

    {
        // Wait for an already-entered FeedAudio call before releasing VADs.
        // The recognizer stays resident; the next duplex turn only rebuilds a stream.
        std::lock_guard<std::mutex> stateLock(g_bargeInMutex);
        if (g_asrStream) {
            SherpaOnnxDestroyOnlineStream(g_asrStream);
            g_asrStream = nullptr;
        }
        if (g_vad) {
            SherpaOnnxDestroyVoiceActivityDetector(g_vad);
            g_vad = nullptr;
        }
        if (g_bargeInVad) {
            SherpaOnnxDestroyVoiceActivityDetector(g_bargeInVad);
            g_bargeInVad = nullptr;
        }
        g_vadLoaded.store(false);
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
    uint64_t generation;
};

// --- 传递给 JS 回调的音频数据 ---
struct TtsAudioChunk {
    int16_t* pcmData;   // PCM int16 数据（由 new[] 分配）
    int32_t sampleCount;
    float progress;
    int32_t sampleRate;  // 模型的实际采样率
    uint64_t generation;
};

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

// Each request owns its JS callback. The callback itself is invoked synchronously
// on the generation thread, so thread_local state avoids cross-round leakage.
static std::atomic<uint64_t> g_ttsGeneration{1};
static thread_local TtsCallbackContext* t_streamingCtx = nullptr;
static thread_local int t_streamingChunkCount = 0;
static thread_local std::chrono::steady_clock::time_point t_generateStartTime;
static thread_local int32_t t_streamingSampleRate = 0;
static thread_local std::chrono::steady_clock::time_point t_firstChunkTime;
static thread_local bool t_firstChunkArrived = false;
static thread_local int64_t t_streamingTotalSamples = 0;
// AEC 参考信号降采样相位累加器（44100Hz → 16000Hz）

// sherpa-onnx 逐句回调：每生成一句话触发一次
// 返回 1 继续生成，返回 0 停止（sherpa-onnx 约定）
static int32_t StreamingTtsCallback(const float* samples, int32_t n) {
    if (!t_streamingCtx || t_streamingCtx->generation != g_ttsGeneration.load()) return 0;
    if (!samples || n <= 0) return 1;

    // 音频块级背压：最多允许约3秒PCM尚未被Renderer消费。
    // stopTts 会改变 generation 并唤醒等待，避免打断被阻塞。
    {
        std::unique_lock<std::mutex> lock(g_ttsFlowMutex);
        const int64_t maxOutstanding = std::max<int64_t>(t_streamingSampleRate * 3LL, 1);
        g_ttsFlowCv.wait(lock, [&]() {
            return t_streamingCtx->generation != g_ttsGeneration.load() ||
                g_ttsFlowGeneration != t_streamingCtx->generation ||
                g_ttsOutstandingSamples < maxOutstanding;
        });
        if (t_streamingCtx->generation != g_ttsGeneration.load() ||
            g_ttsFlowGeneration != t_streamingCtx->generation) return 0;
        g_ttsOutstandingSamples += n;
    }

    auto now = std::chrono::steady_clock::now();
    if (!t_firstChunkArrived) {
        t_firstChunkArrived = true;
        t_firstChunkTime = now;
        OH_LOG_INFO(LOG_APP, "ohos_TTS_TIMING: first chunk at +0ms");
        auto elapsedFirst = std::chrono::duration_cast<std::chrono::milliseconds>(now - t_generateStartTime).count();
        MetricsCollector::RecordTtsFirstChunk(static_cast<double>(elapsedFirst));
        OH_LOG_INFO(LOG_APP, "OHOS_TTS_PERF 06_CXX_firstChunk tid=%{public}d afterGenerate=%{public}lldms n=%{public}d sr=%{public}d",
                    (int)gettid(), (long long)elapsedFirst, n, t_streamingSampleRate);
        t_generateStartTime = now;
        // 但后续 chunk 使用 g_generateStartTime 已过时，改用绝对时间比较
    } else {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - t_firstChunkTime).count();
        OH_LOG_INFO(LOG_APP, "ohos_TTS_TIMING: chunk #%{public}d at +%{public}lld ms, %{public}d samples",
                    t_streamingChunkCount + 1, (long long)elapsed, n);
    }

    int32_t sr = t_streamingSampleRate;
    t_streamingTotalSamples += n;
    TtsAudioChunk* chunk = new TtsAudioChunk;
    chunk->pcmData = new int16_t[n];
    chunk->sampleCount = n;
    chunk->sampleRate = sr;
    chunk->progress = 0.5f;
    chunk->generation = t_streamingCtx->generation;

    for (int32_t i = 0; i < n; i++) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        chunk->pcmData[i] = (int16_t)(s * 32767.0f);
    }

    ++t_streamingChunkCount;
    OH_LOG_INFO(LOG_APP, "ohos_TTS streaming chunk #%{public}d: %{public}d samples",
                t_streamingChunkCount, n);
    OH_LOG_INFO(LOG_APP, "OHOS_fixbug_tts CXX_chunk tid=%{public}d #%{public}d n=%{public}d sr=%{public}d",
                (int)gettid(), t_streamingChunkCount, n, sr);

    if (t_streamingCtx && t_streamingCtx->tsfn) {
        napi_status postStatus =
            napi_call_threadsafe_function(t_streamingCtx->tsfn, chunk, napi_tsfn_blocking);
        if (postStatus != napi_ok) {
            {
                std::lock_guard<std::mutex> lock(g_ttsFlowMutex);
                g_ttsOutstandingSamples = std::max<int64_t>(0, g_ttsOutstandingSamples - n);
            }
            g_ttsFlowCv.notify_all();
            delete[] chunk->pcmData;
            delete chunk;
            return 0;
        }
    } else {
        delete[] chunk->pcmData;
        delete chunk;
    }
    return 1; // 继续生成下一句（sherpa-onnx 约定：1=继续，0=停止）
}

static bool FileExists(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        OH_LOG_ERROR(LOG_APP, "ohos_TTS FileExists: NOT found: %{public}s", path.c_str());
        return false;
    }
    fclose(file);
    return true;
}

// 初始化或复用 TTS 引擎
static bool EnsureTtsEngine(const std::string& modelDir) {
    std::lock_guard<std::mutex> lock(g_ttsMutex);
    if (g_ttsEngine && g_ttsModelDir == modelDir) return true;
    if (g_ttsEngine) {
        SherpaOnnxDestroyOfflineTts(g_ttsEngine);
        g_ttsEngine = nullptr;
        g_ttsModelDir.clear();
    }

    const std::string modelPath = modelDir + "/model.onnx";
    const std::string tokensPath = modelDir + "/tokens.txt";
    const std::string lexiconPath = modelDir + "/lexicon.txt";
    const bool hasLexicon = FileExists(lexiconPath);
    if (!FileExists(modelPath) || !FileExists(tokensPath)) {
        OH_LOG_ERROR(LOG_APP, "ohos_TTS_ENGINE: model files not found in %{public}s", modelDir.c_str());
        return false;
    }

    SherpaOnnxOfflineTtsVitsModelConfig vits{};
    vits.model = modelPath.c_str();
    vits.tokens = tokensPath.c_str();
    vits.lexicon = hasLexicon ? lexiconPath.c_str() : "";
    vits.data_dir = "";
    vits.noise_scale = 0.667f;
    vits.noise_scale_w = 0.8f;
    vits.length_scale = 1.0f;

    SherpaOnnxOfflineTtsModelConfig model{};
    model.vits = vits;
    model.num_threads = g_ttsNumThreads.load();
    model.debug = 1;
    model.provider = "cpu";

    SherpaOnnxOfflineTtsConfig config{};
    config.model = model;
    config.max_num_sentences = 2;

    const auto start = std::chrono::steady_clock::now();
    g_ttsEngine = SherpaOnnxCreateOfflineTts(&config);
    const auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (!g_ttsEngine) {
        OH_LOG_ERROR(LOG_APP, "ohos_TTS_ENGINE: SherpaOnnxCreateOfflineTts FAILED");
        return false;
    }
    g_ttsModelDir = modelDir;
    MetricsCollector::Record(MetricType::TtsThreadCount, static_cast<double>(model.num_threads));
    OH_LOG_INFO(LOG_APP, "ohos_TTS_ENGINE: created in %{public}lld ms, sr=%{public}d",
                static_cast<long long>(loadMs), SherpaOnnxOfflineTtsSampleRate(g_ttsEngine));
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

    if (ctx->generation != g_ttsGeneration.load()) {
        OH_LOG_INFO(LOG_APP, "ohos_TTS: request cancelled before generation");
        ctx->isGenerating->store(false);
        napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
        delete ctx->isGenerating;
        delete ctx;
        return;
    }

    g_ttsMutex.lock();
    const SherpaOnnxOfflineTts* tts = g_ttsEngine;
    if (!tts) {
        g_ttsMutex.unlock();
        OH_LOG_ERROR(LOG_APP, "ohos_TTS engine disappeared before generation");
        ctx->isGenerating->store(false);
        napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
        delete ctx->isGenerating;
        delete ctx;
        return;
    }

    t_streamingCtx = ctx;
    t_streamingSampleRate = SherpaOnnxOfflineTtsSampleRate(tts);
    t_streamingChunkCount = 0;
    t_streamingTotalSamples = 0;
    t_firstChunkArrived = false;
    t_generateStartTime = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_ttsFlowMutex);
        g_ttsFlowGeneration = ctx->generation;
        g_ttsOutstandingSamples = 0;
    }

    OH_LOG_INFO(LOG_APP, "ohos_TTS generating streaming for: %{public}s", text.c_str());
    PerformanceProfiler::Start("TTS Generation");
    auto ttsStart = std::chrono::steady_clock::now();

    const SherpaOnnxGeneratedAudio* fullAudio = SherpaOnnxOfflineTtsGenerateWithCallback(
        tts, text.c_str(), 4, speed, StreamingTtsCallback);

    auto ttsEnd = std::chrono::steady_clock::now();
    auto ttsMs = std::chrono::duration_cast<std::chrono::milliseconds>(ttsEnd - ttsStart).count();
    PerformanceProfiler::End("TTS Generation");
    MetricsCollector::RecordTtsGeneration(ttsMs);
    const double audioLengthMs = t_streamingSampleRate > 0
        ? static_cast<double>(t_streamingTotalSamples) * 1000.0 / t_streamingSampleRate
        : 0.0;
    if (audioLengthMs > 0.0) {
        MetricsCollector::Record(MetricType::TtsAudioLengthMs, audioLengthMs);
        MetricsCollector::RecordTtsRtf(static_cast<double>(ttsMs) / audioLengthMs);
    }
    MetricsCollector::RecordProcessMemorySnapshot();
    g_ttsMutex.unlock();

    const bool cancelled = ctx->generation != g_ttsGeneration.load();
    if (!cancelled) {
        TtsAudioChunk* done = new TtsAudioChunk;
        done->pcmData = nullptr;
        done->sampleCount = 0;
        done->progress = 2.0f;
        done->sampleRate = t_streamingSampleRate;
        done->generation = ctx->generation;
        napi_call_threadsafe_function(ctx->tsfn, done, napi_tsfn_blocking);
    } else {
        OH_LOG_INFO(LOG_APP, "ohos_TTS: generation %{public}llu cancelled after %{public}d chunks",
            static_cast<unsigned long long>(ctx->generation), t_streamingChunkCount);
    }

    OH_LOG_INFO(LOG_APP, "ohos_TTS streaming done: %{public}d chunks, %{public}lld ms",
                t_streamingChunkCount, (long long)ttsMs);
    OH_LOG_INFO(LOG_APP, "OHOS_fixbug_tts CXX_done tid=%{public}d chunks=%{public}d",
                (int)gettid(), t_streamingChunkCount);

    if (fullAudio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(fullAudio);
    // 不销毁引擎，复用
    t_streamingCtx = nullptr;
    ctx->isGenerating->store(false);
    napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
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
    napi_value jsGeneration;
    napi_value jsSampleCount;

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
    napi_create_int64(env, static_cast<int64_t>(chunk->generation), &jsGeneration);
    // Expose the count separately. Some Ark runtime builds expose a native
    // ArrayBuffer wrapper to main-thread NAPI callbacks which cannot safely
    // service property access such as data.byteLength.
    napi_create_int32(env, static_cast<int32_t>(chunk->sampleCount), &jsSampleCount);

    napi_value argv[5] = { jsArrayBuffer, jsProgress, jsSampleRate, jsGeneration, jsSampleCount };
    napi_call_function(env, undefined, jsCallback, 5, argv, nullptr);
    delete chunk;
}

// --- 5. NAPI 导出函数 ---

// ============================================================
// RawFile 复制 — 从 HAP rawfile 流式复制到沙箱目录
// C++ 侧分块读取，避免 ArkTS 大内存分配导致 OOM
// ============================================================
static napi_value CopyRawFile(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) { napi_value r; napi_get_boolean(env, false, &r); return r; }

    size_t len;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string rawPath(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &rawPath[0], len + 1, &len);
    rawPath.resize(len);

    napi_get_value_string_utf8(env, argv[1], nullptr, 0, &len);
    std::string destPath(len, '\0');
    napi_get_value_string_utf8(env, argv[1], &destPath[0], len + 1, &len);
    destPath.resize(len);

    OH_LOG_INFO(LOG_APP, "ohos_COPY: %{public}s", rawPath.c_str());
    bool success = false;

    if (!g_nativeResMgr) {
        OH_LOG_ERROR(LOG_APP, "ohos_COPY: nativeResMgr not initialized");
        napi_value r; napi_get_boolean(env, false, &r); return r;
    }

    RawFile* rawFile = OH_ResourceManager_OpenRawFile(g_nativeResMgr, rawPath.c_str());
    if (!rawFile) {
        OH_LOG_ERROR(LOG_APP, "ohos_COPY: rawfile not found: %{public}s", rawPath.c_str());
        napi_value r; napi_get_boolean(env, false, &r); return r;
    }

    // 创建目录
    auto slash = destPath.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = destPath.substr(0, slash);
        size_t pos = 0;
        while ((pos = dir.find_first_of('/', pos + 1)) != std::string::npos) {
            mkdir(dir.substr(0, pos).c_str(), 0755);
        }
        mkdir(dir.c_str(), 0755);
    }

    FILE* dest = fopen(destPath.c_str(), "wb");
    if (!dest) {
        OH_LOG_ERROR(LOG_APP, "ohos_COPY: cannot open: %{public}s", destPath.c_str());
        OH_ResourceManager_CloseRawFile(rawFile);
        napi_value r; napi_get_boolean(env, false, &r); return r;
    }

    const size_t CHUNK = 65536; // 64KB
    char buf[CHUNK];
    long total = 0;

    while (true) {
        long n = OH_ResourceManager_ReadRawFile(rawFile, buf, CHUNK);
        if (n <= 0) break;
        fwrite(buf, 1, n, dest);
        total += n;
    }

    fclose(dest);
    OH_ResourceManager_CloseRawFile(rawFile);
    OH_LOG_INFO(LOG_APP, "ohos_COPY: done");
    success = (total > 0);

    napi_value result;
    napi_get_boolean(env, success, &result);
    return result;
}

// pingNative 实现
static napi_value PingNative(napi_env env, napi_callback_info info) {
    napi_value result;
    napi_create_int32(env, 42, &result); // 示例返回值
    return result;
}

// ============================================================
// 性能报告生成 — 汇总所有指标为可读文本
// ============================================================
// 面试常问：你的项目性能怎么样？
// 这个函数让你能一键输出性能体检报告，有数据说话。
// ============================================================
static napi_value GetBenchmarkReport(napi_env env, napi_callback_info info) {
    std::string report;
    report += "========================================\n";
    report += "  OHOS Native AI Engine — 性能报告\n";
    report += "========================================\n\n";

    // 1. 内存信息（从 /proc/self/status 读取）
    report += "📊 内存信息\n";
    report += "----------------------------------------\n";
    FILE* proc = fopen("/proc/self/status", "r");
    if (proc) {
        char line[256];
        while (fgets(line, sizeof(line), proc)) {
            // 只提取关键的几行：VmRSS, VmHWM, VmSize, Threads
            if (strstr(line, "VmRSS:") || strstr(line, "VmSize:") ||
                strstr(line, "VmHWM:") || strstr(line, "Threads:")) {
                // 去掉末尾换行
                size_t len = strlen(line);
                if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
                report += "  " + std::string(line) + "\n";
            }
        }
        fclose(proc);
    } else {
        report += "  (无法读取 /proc/self/status)\n";
    }
    report += "\n";

    // 2. 各指标统计摘要
    report += "📊 性能指标统计\n";
    report += "----------------------------------------\n";
    report += "  指标名称               均值    最小    最大    样本数\n";
    report += "  -----------------------------------------------\n";

    // 遍历所有指标类型
    for (int i = 0; i < static_cast<int>(MetricType::Count); i++) {
        MetricType type = static_cast<MetricType>(i);
        MetricSummary s = MetricsCollector::GetSummary(type);
        if (s.count == 0) continue; // 无数据就跳过

        std::string name = MetricTypeToString(type);
        name.resize(24, ' '); // 左对齐填充

        char line[256];
        snprintf(line, sizeof(line), "  %s %8.2f %8.2f %8.2f %8u\n",
                 name.c_str(), s.avg, s.min, s.max, s.count);
        report += line;
    }
    report += "\n";

    // 3. 综合评估
    report += "📊 综合评估\n";
    report += "----------------------------------------\n";

    MetricSummary rtfSummary = MetricsCollector::GetSummary(MetricType::AsrRtf);
    if (rtfSummary.count > 0) {
        std::string rtfStatus = rtfSummary.avg < 0.3f ? "✅ 优秀" : "⚠️ 偏高";
        report += "  ASR RTF:       " + rtfStatus +
                  " (均值 " + std::to_string(rtfSummary.avg) + ")\n";
    }
    MetricSummary pipelineSummary = MetricsCollector::GetSummary(MetricType::AudioPipelineTotalMs);
    if (pipelineSummary.count > 0) {
        report += "  音频管线延迟:   " + std::to_string(pipelineSummary.avg) + " ms\n";
    }
    MetricSummary memSummary = MetricsCollector::GetSummary(MetricType::DcFilterLatencyUs);
    if (memSummary.count > 0) {
        report += "  DC 滤波:        " + std::to_string(memSummary.avg) + " us\n";
    }

    report += "\n========================================\n";
    report += "  报告生成时间: ";
    // 简单的时间戳
    auto now = std::chrono::system_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    report += std::to_string(now_ms);
    report += "\n========================================\n";

    napi_value result;
    napi_create_string_utf8(env, report.c_str(), NAPI_AUTO_LENGTH, &result);
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
                    g_ttsEngine ? SherpaOnnxOfflineTtsGenerate(g_ttsEngine, "。", 0, 1.0f) : nullptr;
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
    ctx->generation = g_ttsGeneration.load();
    OH_LOG_INFO(LOG_APP, "ohos_TTS ctx: text=%{public}s, modelDir=%{public}s, speed=%.1f",
                text.c_str(), modelDir.c_str(), speed);
    napi_value resource_name;
    napi_create_string_utf8(env, "ttsCallback", NAPI_AUTO_LENGTH, &resource_name);
    // Every request gets its own callback. Reusing the first request's callback
    // causes all chunks after an interruption to be rejected as stale.
    napi_status tsfnStatus = napi_create_threadsafe_function(env, argv[3], nullptr, resource_name,
        0, 1, nullptr, nullptr, nullptr, CallJsCallback, &ctx->tsfn);
    if (tsfnStatus != napi_ok) {
        delete ctx->isGenerating;
        delete ctx;
        return nullptr;
    }
    std::thread(BackgroundTtsThread, ctx).detach();
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value StopTts(napi_env env, napi_callback_info info) {
    const uint64_t cancelledGeneration = g_ttsGeneration.fetch_add(1);
    g_ttsFlowCv.notify_all();
    OH_LOG_INFO(LOG_APP, "ohos_TTS: cancel generation %{public}llu",
        static_cast<unsigned long long>(cancelledGeneration));
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// Release only the resident TTS model. Callers must ensure no new playback is
// being scheduled; the mutex waits for an already-running generation to leave.
static napi_value ReleaseTtsEngine(napi_env env, napi_callback_info info) {
    (void)info;
    if (g_ttsLifecycleLeaseCount.load() > 0) {
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
    }
    g_ttsGeneration.fetch_add(1);
    g_ttsFlowCv.notify_all();
    bool released = false;
    {
        std::lock_guard<std::mutex> lock(g_ttsMutex);
        if (g_ttsEngine) {
            SherpaOnnxDestroyOfflineTts(g_ttsEngine);
            g_ttsEngine = nullptr;
            g_ttsModelDir.clear();
            released = true;
        }
    }
    // Do not carry an old renderer acknowledgement / backpressure balance into
    // a lazily-created engine on the next turn.
    {
        std::lock_guard<std::mutex> lock(g_ttsFlowMutex);
        g_ttsOutstandingSamples = 0;
        g_ttsFlowGeneration = g_ttsGeneration.load();
    }
    napi_value result;
    napi_get_boolean(env, released, &result);
    return result;
}

static napi_value BeginTtsLifecycleLease(napi_env env, napi_callback_info info) {
    (void)info;
    g_ttsLifecycleLeaseCount.fetch_add(1);
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value EndTtsLifecycleLease(napi_env env, napi_callback_info info) {
    (void)info;
    int32_t current = g_ttsLifecycleLeaseCount.load();
    while (current > 0 && !g_ttsLifecycleLeaseCount.compare_exchange_weak(current, current - 1)) {}
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// Renderer完成一个PCM块后回执，解除native TTS生成线程的块级背压。
static napi_value AckTtsChunk(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t samples = 0;
    int64_t generation = 0;
    if (argc >= 1) napi_get_value_int32(env, argv[0], &samples);
    if (argc >= 2) napi_get_value_int64(env, argv[1], &generation);
    if (samples > 0) {
        std::lock_guard<std::mutex> lock(g_ttsFlowMutex);
        if (static_cast<uint64_t>(generation) == g_ttsFlowGeneration) {
            g_ttsOutstandingSamples =
                std::max<int64_t>(0, g_ttsOutstandingSamples - samples);
        }
    }
    g_ttsFlowCv.notify_all();
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

    // 注册状态机 → ArkTS 动作回调（使用 PipelineDispatcher 类型化路由）
    {
        // 创建全局 PipelineDispatcher（如果尚未创建）
        static PipelineDispatcher s_pipelineDispatcher;
        s_pipelineDispatcher.Clear();

        // 注册 C++ 侧直接处理的事件（不经过 ArkTS）
        s_pipelineDispatcher.Register(PipelineEvent::kInterrupted, [](const PipelineEvent& e) {
            OH_LOG_INFO(LOG_APP, "ohos_FD interrupt: clearing ASR + cancelling LLM");
            g_audioRingBuffer.Clear();
            g_asrResetRequested.store(true);
            std::lock_guard<std::mutex> engineLock(g_localLlmMutex);
            if (g_localLlmEngine) {
                OH_LOG_INFO(LOG_APP, "ohos_FD cancelling local LLM generation");
                g_localLlmEngine->CancelGeneration();
            }
        });

        // 全双工重叠模式：用户打断时保存 AI 的部分回复
        s_pipelineDispatcher.Register(PipelineEvent::kOverlapStart, [](const PipelineEvent& e) {
            OH_LOG_INFO(LOG_APP, "ohos_FD overlap START: saving partial AI response");
            // 不要清除 RingBuffer！保留用户输入的音频供 ASR 继续解码
            // 转发到 ArkTS
            if (g_duplexActionTsFunction) {
                auto* data = new DuplexActionData{"overlap_start", e.payload};
                napi_call_threadsafe_function(g_duplexActionTsFunction, data, napi_tsfn_nonblocking);
            }
        });

        // 全双工重叠结束：用户说完了，保存上下文
        s_pipelineDispatcher.Register(PipelineEvent::kOverlapEnd, [](const PipelineEvent& e) {
            OH_LOG_INFO(LOG_APP, "ohos_FD overlap END: user finished, pending ASR processing");
            if (g_duplexActionTsFunction) {
                auto* data = new DuplexActionData{"overlap_end", e.payload};
                napi_call_threadsafe_function(g_duplexActionTsFunction, data, napi_tsfn_nonblocking);
            }
        });

        // 注册转发到 ArkTS TSFN 的事件处理器
        auto forwardToArkTS = [](const PipelineEvent& e) {
            if (g_duplexActionTsFunction) {
                auto* data = new DuplexActionData{
                    PipelineEventTypeToString(e.type),
                    e.payload
                };
                napi_call_threadsafe_function(g_duplexActionTsFunction, data, napi_tsfn_nonblocking);
            }
        };

        s_pipelineDispatcher.Register(PipelineEvent::kLlmRequest, forwardToArkTS);
        s_pipelineDispatcher.Register(PipelineEvent::kAsrInterim, forwardToArkTS);
        s_pipelineDispatcher.Register(PipelineEvent::kStopTts, forwardToArkTS);
        s_pipelineDispatcher.Register(PipelineEvent::kDuckTts, forwardToArkTS);
        s_pipelineDispatcher.Register(PipelineEvent::kResumeTts, forwardToArkTS);

        s_pipelineDispatcher.Register(PipelineEvent::kLlmStart, forwardToArkTS);
        s_pipelineDispatcher.Register(PipelineEvent::kLlmComplete, forwardToArkTS);
        s_pipelineDispatcher.Register(PipelineEvent::kTtsStart, forwardToArkTS);
        s_pipelineDispatcher.Register(PipelineEvent::kTtsComplete, forwardToArkTS);

        // 状态机回调：将字符串 action 转为 PipelineEvent 分派
        g_audioStateMachine.SetOnActionRequest(
            [](const std::string& action, const std::string& payload) {
                // 字符串 → PipelineEvent::Type 映射表
                static const std::unordered_map<std::string, PipelineEvent::Type> s_actionMap = {
                    {"llm_request",        PipelineEvent::kLlmRequest},
                    {"llm_start",          PipelineEvent::kLlmStart},
                    {"llm_complete",       PipelineEvent::kLlmComplete},
                    {"asr_interim",        PipelineEvent::kAsrInterim},
                    {"asr_final",          PipelineEvent::kAsrFinal},
                    {"stop_tts",           PipelineEvent::kStopTts},
                    {"duck_tts",           PipelineEvent::kDuckTts},
                    {"resume_tts",         PipelineEvent::kResumeTts},
                    {"tts_started",        PipelineEvent::kTtsStart},
                    {"tts_complete",       PipelineEvent::kTtsComplete},

                    {"interrupted",        PipelineEvent::kInterrupted},

                };

                auto it = s_actionMap.find(action);
                if (it != s_actionMap.end()) {
                    s_pipelineDispatcher.Dispatch(PipelineEvent(it->second, payload));
                } else {
                    OH_LOG_WARN(LOG_APP, "ohos_FD unknown action: %{public}s", action.c_str());
                }
            }
        );
    }
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

// 开发性能对比：切换 2/4 线程。销毁旧引擎，下一次生成会按新配置重建。
static napi_value SetTtsNumThreads(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t requested = 4;
    if (argc > 0) napi_get_value_int32(env, argv[0], &requested);
    if (requested != 2 && requested != 4) requested = 4;
    const int32_t previous = g_ttsNumThreads.exchange(requested);
    if (previous != requested) {
        std::lock_guard<std::mutex> lock(g_ttsMutex);
        if (g_ttsEngine) {
            SherpaOnnxDestroyOfflineTts(g_ttsEngine);
            g_ttsEngine = nullptr;
            g_ttsModelDir.clear();
        }
    }
    MetricsCollector::Record(MetricType::TtsThreadCount, static_cast<double>(requested));
    OH_LOG_INFO(LOG_APP, "ohos_TTS_ENGINE: thread config=%{public}d", requested);
    napi_value result;
    napi_create_int32(env, requested, &result);
    return result;
}

static void CompleteBargeInHandoff() {
    if (!g_bargeInHandoffPending.load()) {
        // Manual interruption has no candidate buffer, but still needs the
        // renderer acknowledgement to leave INTERRUPTED.
        g_audioStateMachine.OnBargeInTtsStopped();
        return;
    }

    size_t candidateSamples = 0;
    {
        std::lock_guard<std::mutex> lock(g_bargeInMutex);
        if (!g_bargeInHandoffPending.load()) return;

        candidateSamples = g_feedAudioState.bargeInCandidateAudio.size();
        g_audioRingBuffer.Clear();
        if (!g_feedAudioState.bargeInCandidateAudio.empty()) {
            g_audioRingBuffer.Write(g_feedAudioState.bargeInCandidateAudio.data(), candidateSamples);
        }
        g_feedAudioState.vadAccumulator.clear();
        g_feedAudioState.bargeInVadAccumulator.clear();
        g_feedAudioState.bargeInPreRoll.clear();
        g_feedAudioState.bargeInCandidateAudio.clear();
        g_feedAudioState.bargeInCandidateActive = false;
        g_feedAudioState.bargeInVoiceFrames = 0;
        g_feedAudioState.bargeInWindowFrames = 0;
        g_feedAudioState.bargeInMissFrames = 0;
        g_feedAudioState.prevTtsPlaying = false;
        g_feedAudioState.ttsEndTime = std::chrono::steady_clock::time_point();
        g_bargeInHandoffPending.store(false);
    }

    OH_LOG_INFO(LOG_APP, "ohos_BARGE_HANDOFF: renderer stopped, handing %{public}zu samples to ASR",
        candidateSamples);
    g_audioStateMachine.OnBargeInTtsStopped();
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
    else if (evt == "tts_complete") {
        {
            std::lock_guard<std::mutex> lock(g_bargeInMutex);
            g_feedAudioState.bargeInVadAccumulator.clear();
            g_feedAudioState.bargeInPreRoll.clear();
            g_feedAudioState.bargeInCandidateAudio.clear();
            g_feedAudioState.bargeInCandidateActive = false;
            g_feedAudioState.bargeInVoiceFrames = 0;
            g_feedAudioState.bargeInWindowFrames = 0;
            g_feedAudioState.bargeInMissFrames = 0;
        }
        g_audioStateMachine.OnTtsComplete();
    }
    else if (evt == "tts_stopped") CompleteBargeInHandoff();
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

// ============================================================
// Benchmark 引擎 — 主动压测 + 返回结构化 JSON 结果
// ============================================================
static napi_value RunBenchmark(napi_env env, napi_callback_info info) {
    BenchmarkEngine bench;
    std::string json = bench.RunAll();

    napi_value result;
    napi_create_string_utf8(env, json.c_str(), NAPI_AUTO_LENGTH, &result);
    return result;
}

// 重置所有指标
static napi_value ResetMetrics(napi_env env, napi_callback_info info) {
    MetricsCollector::Reset();
    // Persist the empty session immediately; otherwise a process killed before
    // the next autosave would load the previous process's metrics again.
    MetricsCollector::SaveToDisk();
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
    g_asrResetRequested.store(true);
    OH_LOG_INFO(LOG_APP, "ohos_ASR stream reset queued for decoder thread");
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// ============================================================
// 本地 LLM 推理（Qwen2.5-0.5B + ONNX Runtime）
// ============================================================

/// g_localLlmEngine 声明在文件头部（约第 55 行），
/// 因为 interrupt handler lambda（约第 1415 行）中先使用了它。
/// 这里只保留注释说明，防止重复声明。
// 全局本地 LLM 引擎实例 — 见文件头部声明

// 每个推理请求持有独立 TSFN。全局 TSFN 会在打断/快速追问产生请求交叠时被
// 后一个请求覆盖，导致旧结果投递给新一轮回调。
struct LlmCallbackContext {
    napi_threadsafe_function token = nullptr;
    napi_threadsafe_function complete = nullptr;
};

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

// NAPI: initLocalLlm(modelDir) → boolean
// 初始化本地 LLM 引擎，由 ArkTS 在切换本地模式时调用
static napi_value InitLocalLlm(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        napi_value r; napi_get_boolean(env, false, &r); return r;
    }

    size_t len;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    std::string modelDir(len, '\0');
    napi_get_value_string_utf8(env, argv[0], &modelDir[0], len + 1, &len);
    modelDir.resize(len);

    OH_LOG_INFO(LOG_APP, "ohos_LLM: initLocalLlm, modelDir=%{public}s", modelDir.c_str());

    std::lock_guard<std::mutex> engineLock(g_localLlmMutex);

    // 清理旧引擎
    if (g_localLlmEngine) {
        g_localLlmEngine->Release();
        g_localLlmEngine.reset();
    }

    LlmConfig cfg;
    const std::string ggufPath = modelDir + "/model.gguf";
    FILE* ggufFile = fopen(ggufPath.c_str(), "rb");
    if (!ggufFile) {
        OH_LOG_ERROR(LOG_APP, "ohos_LLM: GGUF model is not installed");
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
    }
    fclose(ggufFile);
    OH_LOG_INFO(LOG_APP, "ohos_LLM: selecting GGUF backend");
    g_localLlmEngine = std::make_unique<GgufLlmEngine>();
    cfg.modelPath = ggufPath;
    // 端侧语音助手优先保证可预期的整轮耗时。160 tokens 足以覆盖默认
    // 2~4 句话，同时避免偶发 300+ token 回复阻塞十几秒并污染后续 prefill。
    cfg.maxTokens = 160;
    // 小模型优先稳定与可复现，降低随机采样造成的事实型幻觉。
    cfg.temperature = 0.25f;

    bool ok = g_localLlmEngine->Init(cfg);
    napi_value result;
    napi_get_boolean(env, ok, &result);

    if (!ok) {
        OH_LOG_ERROR(LOG_APP, "ohos_LLM: initLocalLlm FAILED");
        g_localLlmEngine.reset();
    }
    return result;
}

static std::vector<std::pair<std::string, std::string>> ParseLlmHistory(
    const std::string& historyJson) {
    std::vector<std::pair<std::string, std::string>> messages;
    if (historyJson.empty()) return messages;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(historyJson);
        if (!parsed.is_array()) return messages;
        constexpr size_t kMaxHistoryMessages = 21;
        const size_t start = parsed.size() > kMaxHistoryMessages
            ? parsed.size() - kMaxHistoryMessages : 0;
        for (size_t i = start; i < parsed.size(); ++i) {
            const auto& item = parsed[i];
            if (!item.is_object() || !item.contains("role") || !item.contains("content") ||
                !item["role"].is_string() || !item["content"].is_string()) {
                continue;
            }
            std::string role = item["role"].get<std::string>();
            std::string content = item["content"].get<std::string>();
            if (role != "system" && role != "user" && role != "assistant") continue;
            if (content.empty()) continue;
            if (content.size() > 8192) content.resize(8192);
            messages.push_back({std::move(role), std::move(content)});
        }
    } catch (const std::exception& e) {
        OH_LOG_WARN(LOG_APP, "ohos_LLM: invalid history JSON: %{public}s", e.what());
    }
    return messages;
}

// Diagnostic-only: force the next local GGUF call to decode a fixed number
// of non-EOS tokens. Production calls remain unchanged unless this is invoked.
static napi_value SetLocalLlmDiagnosticTokens(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t requested = 0;
    if (argc > 0) napi_get_value_int32(env, argv[0], &requested);
    const int32_t applied = std::max(0, std::min(requested, 256));
    {
        std::lock_guard<std::mutex> engineLock(g_localLlmMutex);
        if (g_localLlmEngine && g_localLlmEngine->IsInitialized()) {
            g_localLlmEngine->SetDiagnosticDecodeTokens(applied);
        } else {
            requested = 0;
        }
    }
    napi_value result;
    napi_create_int32(env, requested == 0 ? 0 : applied, &result);
    return result;
}

// NAPI: callLocalLlm(text, historyJson, onToken, onComplete)
// 流式调用本地 LLM（需先调用 initLocalLlm）
static napi_value CallLocalLlm(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) return nullptr;

    // Keep the instance alive until CallStreaming registers the async worker.
    std::unique_lock<std::mutex> engineLock(g_localLlmMutex);

    // 引擎未初始化
    if (!g_localLlmEngine || !g_localLlmEngine->IsInitialized()) {
        OH_LOG_ERROR(LOG_APP, "ohos_LLM: callLocalLlm but engine not initialized");
        // 通过完成回调返回错误
        if (argc >= 4) {
            LlmCompleteData* data = new LlmCompleteData{"Local LLM not initialized. Call initLocalLlm first.", false};
            // 直接同步调用 complete callback
            napi_value undefined, jsText, jsSuccess;
            napi_get_undefined(env, &undefined);
            napi_create_string_utf8(env, data->text.c_str(), NAPI_AUTO_LENGTH, &jsText);
            napi_get_boolean(env, false, &jsSuccess);
            napi_value argv2[2] = { jsText, jsSuccess };
            napi_call_function(env, undefined, argv[3], 2, argv2, nullptr);
            delete data;
        }
        napi_value r; napi_get_undefined(env, &r); return r;
    }

    // 解析 text
    size_t textLen = 0;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &textLen);
    std::vector<char> textBuffer(textLen + 1, '\0');
    napi_get_value_string_utf8(env, argv[0], textBuffer.data(), textBuffer.size(), &textLen);
    std::string text(textBuffer.data(), textLen);

    size_t historyLen = 0;
    napi_get_value_string_utf8(env, argv[1], nullptr, 0, &historyLen);
    std::string historyJson;
    if (historyLen > 0) {
        std::vector<char> historyBuffer(historyLen + 1, '\0');
        napi_get_value_string_utf8(env, argv[1], historyBuffer.data(), historyBuffer.size(), &historyLen);
        historyJson.assign(historyBuffer.data(), historyLen);
    }

    // 创建 TSFN 回调
    napi_value resource_name;
    napi_create_string_utf8(env, "llmTokenCallback", NAPI_AUTO_LENGTH, &resource_name);
    auto callbackContext = std::make_shared<LlmCallbackContext>();
    napi_create_threadsafe_function(env, argv[2], nullptr, resource_name,
        0, 1, nullptr, nullptr, nullptr, LlmTokenCallJs, &callbackContext->token);
    
    napi_create_string_utf8(env, "llmCompleteCallback", NAPI_AUTO_LENGTH, &resource_name);
    napi_create_threadsafe_function(env, argv[3], nullptr, resource_name,
        0, 1, nullptr, nullptr, nullptr, LlmCompleteCallJs, &callbackContext->complete);

    // 构建 messages
    std::vector<std::pair<std::string, std::string>> messages = ParseLlmHistory(historyJson);
    messages.push_back({"user", text});
    OH_LOG_INFO(LOG_APP, "ohos_LLM: local request context messages=%{public}zu", messages.size());

    // 流式调用
    g_localLlmEngine->CallStreaming(
        messages,
        [callbackContext](const std::string& token) -> bool {
            if (callbackContext->token) {
                auto* data = new LlmTokenData{token};
                const napi_status status = napi_call_threadsafe_function(
                    callbackContext->token, data, napi_tsfn_nonblocking);
                if (status != napi_ok) {
                    delete data;
                    return false;
                }
            }
            return true;
        },
        [callbackContext](const std::string& fullText, bool success) {
            if (callbackContext->complete) {
                auto* data = new LlmCompleteData{fullText, success};
                const napi_status status = napi_call_threadsafe_function(
                    callbackContext->complete, data, napi_tsfn_blocking);
                if (status != napi_ok) delete data;
            }
            // 清理 TSFN
            if (callbackContext->token) {
                napi_release_threadsafe_function(callbackContext->token, napi_tsfn_release);
                callbackContext->token = nullptr;
            }
            if (callbackContext->complete) {
                napi_release_threadsafe_function(callbackContext->complete, napi_tsfn_release);
                callbackContext->complete = nullptr;
            }
        }
    );
    engineLock.unlock();

    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

// NAPI: isLocalLlmAvailable(modelDir) → boolean
// 检查本地 LLM 模型文件是否存在
static napi_value IsLocalLlmAvailable(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    bool available = false;
    if (argc >= 1) {
        size_t len;
        napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
        std::string modelDir(len, '\0');
        napi_get_value_string_utf8(env, argv[0], &modelDir[0], len + 1, &len);
        modelDir.resize(len);

        const std::string ggufPath = modelDir + "/model.gguf";
        FILE* f = fopen(ggufPath.c_str(), "rb");
        if (f) { fclose(f); available = true; }
    }

    // 也检查引擎是否已初始化
    std::lock_guard<std::mutex> engineLock(g_localLlmMutex);
    if (g_localLlmEngine && g_localLlmEngine->IsInitialized()) {
        available = true;
    }

    napi_value result;
    napi_get_boolean(env, available, &result);
    return result;
}

// Cancels active generation before releasing the large session/model buffers.
// Repeated calls are safe.
static napi_value ReleaseLocalLlm(napi_env env, napi_callback_info info) {
    (void)info;
    std::lock_guard<std::mutex> engineLock(g_localLlmMutex);
    if (g_localLlmEngine) {
        OH_LOG_INFO(LOG_APP, "ohos_LLM: releaseLocalLlm");
        g_localLlmEngine->CancelGeneration();
        g_localLlmEngine->Release();
        g_localLlmEngine.reset();
    }
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value GetRuntimeMemorySnapshot(napi_env env, napi_callback_info info) {
    (void)info;
    long rssKb = 0;
    long peakKb = 0;
    long threads = 0;
    FILE* proc = fopen("/proc/self/status", "r");
    if (proc) {
        char line[256];
        while (fgets(line, sizeof(line), proc)) {
            if (sscanf(line, "VmRSS: %ld kB", &rssKb) == 1) continue;
            if (sscanf(line, "VmHWM: %ld kB", &peakKb) == 1) continue;
            if (sscanf(line, "Threads: %ld", &threads) == 1) continue;
        }
        fclose(proc);
    }

    bool ttsLoaded = false;
    {
        std::lock_guard<std::mutex> lock(g_ttsMutex);
        ttsLoaded = g_ttsEngine != nullptr;
    }
    bool llmLoaded = false;
    {
        std::lock_guard<std::mutex> lock(g_localLlmMutex);
        llmLoaded = g_localLlmEngine && g_localLlmEngine->IsInitialized();
    }
    nlohmann::json snapshot = {
        {"rssKb", rssKb},
        {"peakKb", peakKb},
        {"threads", threads},
        {"asrLoaded", g_asrPrewarmed.load()},
        {"asrRunning", g_isAsrRunning.load()},
        {"vadLoaded", g_vadLoaded.load()},
        {"ttsLoaded", ttsLoaded},
        {"llmLoaded", llmLoaded}
    };
    const std::string json = snapshot.dump();
    napi_value result;
    napi_create_string_utf8(env, json.c_str(), json.size(), &result);
    return result;
}

// Deterministic regression for rules that are otherwise difficult to exercise
// repeatedly through a physical microphone. This uses the same production
// filler filter and AudioStateMachine implementation; it does not touch the
// process-wide assistant instance.
static napi_value RunVoiceLogicRegression(napi_env env, napi_callback_info info) {
    (void)info;
    nlohmann::json cases = nlohmann::json::array();
    int passed = 0;
    int failed = 0;
    auto addCase = [&](const std::string& name, bool pass, const std::string& detail) {
        cases.push_back({{"name", name}, {"passed", pass}, {"detail", detail}});
        pass ? ++passed : ++failed;
    };

    const std::vector<std::string> discardTexts = {"", " ", "嗯", "啊。", "呃", "哦"};
    for (const auto& text : discardTexts) {
        addCase("filler_discard:" + (text.empty() ? std::string("<empty>") : text),
                ShouldDiscardAsrFinal(text), "expected discard");
    }
    const std::vector<std::string> acceptedTexts = {"你好", "停止播放", "嗯我想问一下", "今天天气"};
    for (const auto& text : acceptedTexts) {
        addCase("speech_accept:" + text, !ShouldDiscardAsrFinal(text), "expected accept");
    }

    AudioStateMachine machine;
    AudioStateMachineConfig config;
    config.debugLog = false;
    config.asrTimeoutMs = 60000;
    config.llmTimeoutMs = 60000;
    std::vector<std::string> actions;
    machine.Init(config);
    machine.SetOnActionRequest([&](const std::string& action, const std::string& payload) {
        actions.push_back(action + ":" + payload);
    });
    machine.Start();
    addCase("state_start_listening", machine.GetCurrentState() == AudioState::LISTENING,
            AudioStateToString(machine.GetCurrentState()));

    machine.OnAsrInterim("你好");
    addCase("interim_does_not_advance", machine.GetCurrentState() == AudioState::LISTENING,
            AudioStateToString(machine.GetCurrentState()));
    machine.OnAsrFinal("你好");
    addCase("final_enters_llm_waiting", machine.GetCurrentState() == AudioState::LLM_WAITING,
            AudioStateToString(machine.GetCurrentState()));
    addCase("final_emits_llm_request",
            std::find(actions.begin(), actions.end(), "llm_request:你好") != actions.end(),
            "actions=" + std::to_string(actions.size()));
    machine.OnLlmComplete("你好，我在。");
    addCase("llm_complete_enters_tts", machine.GetCurrentState() == AudioState::TTS_PLAYING,
            AudioStateToString(machine.GetCurrentState()));
    machine.OnTtsComplete();
    addCase("tts_complete_returns_listening", machine.GetCurrentState() == AudioState::LISTENING,
            AudioStateToString(machine.GetCurrentState()));

    actions.clear();
    machine.OnAsrFinal("讲一个故事");
    machine.OnLlmComplete("从前有一座山。");
    machine.OnVadSpeechStart();
    addCase("vad_opens_barge_candidate",
            machine.GetCurrentState() == AudioState::BARGE_IN_CANDIDATE,
            AudioStateToString(machine.GetCurrentState()));
    machine.OnAsrInterim("停下");
    addCase("speech_confirms_barge_in", machine.GetCurrentState() == AudioState::INTERRUPTED,
            AudioStateToString(machine.GetCurrentState()));
    const bool emittedStop = std::find(actions.begin(), actions.end(), "stop_tts:") != actions.end();
    const bool emittedInterrupt = std::find(actions.begin(), actions.end(), "interrupted:") != actions.end();
    addCase("barge_emits_stop_and_interrupt", emittedStop && emittedInterrupt,
            "stop=" + std::to_string(emittedStop) + ",interrupt=" + std::to_string(emittedInterrupt));
    machine.OnBargeInTtsStopped();
    addCase("renderer_ack_returns_listening", machine.GetCurrentState() == AudioState::LISTENING,
            AudioStateToString(machine.GetCurrentState()));

    machine.OnAsrFinal("继续");
    machine.OnLlmComplete("继续播放。");
    machine.OnVadSpeechStart();
    machine.CancelBargeIn();
    addCase("false_barge_resumes_tts", machine.GetCurrentState() == AudioState::TTS_PLAYING,
            AudioStateToString(machine.GetCurrentState()));
    machine.OnTtsComplete();
    machine.Stop();

    nlohmann::json report = {
        {"suite", "native_voice_logic"},
        {"timestampMs", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()},
        {"passed", passed},
        {"failed", failed},
        {"cases", cases}
    };
    const std::string json = report.dump();
    OH_LOG_INFO(LOG_APP, "OHOS_REGRESSION native voice logic passed=%{public}d failed=%{public}d",
                passed, failed);
    napi_value result;
    napi_create_string_utf8(env, json.c_str(), json.size(), &result);
    return result;
}

// --- 模块注册 ---
EXTERN_C_START
extern "C" napi_value Init(napi_env env, napi_value exports) {
    OH_LOG_INFO(LOG_APP, "ohos_NAPI Init called.");
    napi_property_descriptor descs[] = {
        { "pingNative", 0, PingNative, 0, 0, 0, napi_default, 0 },
        // 性能报告
        { "getBenchmarkReport", 0, GetBenchmarkReport, 0, 0, 0, napi_default, 0 },
        // Benchmark 压测引擎
        { "runBenchmark", 0, RunBenchmark, 0, 0, 0, napi_default, 0 },
        { "startStreamingTtsWithSpeed", 0, StartStreamingTtsWithSpeed, 0, 0, 0, napi_default, 0 },
        { "setTtsNumThreads", 0, SetTtsNumThreads, 0, 0, 0, napi_default, 0 },
        { "stopTts", 0, StopTts, 0, 0, 0, napi_default, 0 },
        { "releaseTtsEngine", 0, ReleaseTtsEngine, 0, 0, 0, napi_default, 0 },
        { "beginTtsLifecycleLease", 0, BeginTtsLifecycleLease, 0, 0, 0, napi_default, 0 },
        { "endTtsLifecycleLease", 0, EndTtsLifecycleLease, 0, 0, 0, napi_default, 0 },
        { "ackTtsChunk", 0, AckTtsChunk, 0, 0, 0, napi_default, 0 },
        { "startAssistant", 0, StartAssistant, 0, 0, 0, napi_default, 0 },
        { "feedAudio", 0, FeedAudio, 0, 0, 0, napi_default, 0 },
        { "stopAssistant", 0, StopAssistant, 0, 0, 0, napi_default, 0 },
        { "setPerfCallback", 0, SetPerfCallback, 0, 0, 0, napi_default, 0 },
        { "initRawfileMgmt", 0, InitRawfileMgmt, 0, 0, 0, napi_default, 0 },
        { "readRawFileSync", 0, ReadRawFileSync, 0, 0, 0, napi_default, 0 },
        { "copyRawFile", 0, CopyRawFile, 0, 0, 0, napi_default, 0 },
        { "setDuplexCallbacks", 0, SetDuplexCallbacks, 0, 0, 0, napi_default, 0 },
        { "startFullDuplex", 0, StartFullDuplex, 0, 0, 0, napi_default, 0 },
        { "stopFullDuplex", 0, StopFullDuplex, 0, 0, 0, napi_default, 0 },
        { "getFullDuplexState", 0, GetFullDuplexState, 0, 0, 0, napi_default, 0 },
        { "notifyDuplexEvent", 0, NotifyDuplexEvent, 0, 0, 0, napi_default, 0 },
        // 打断检测
        { "checkInterruption", 0, CheckInterruption, 0, 0, 0, napi_default, 0 },
        { "resetAsrStream", 0, ResetAsrStream, 0, 0, 0, napi_default, 0 },
        // 本地 LLM
        { "initLocalLlm", 0, InitLocalLlm, 0, 0, 0, napi_default, 0 },
        { "setLocalLlmDiagnosticTokens", 0, SetLocalLlmDiagnosticTokens, 0, 0, 0, napi_default, 0 },
        { "callLocalLlm", 0, CallLocalLlm, 0, 0, 0, napi_default, 0 },
        { "isLocalLlmAvailable", 0, IsLocalLlmAvailable, 0, 0, 0, napi_default, 0 },
        { "releaseLocalLlm", 0, ReleaseLocalLlm, 0, 0, 0, napi_default, 0 },
        { "getRuntimeMemorySnapshot", 0, GetRuntimeMemorySnapshot, 0, 0, 0, napi_default, 0 },
        { "runVoiceLogicRegression", 0, RunVoiceLogicRegression, 0, 0, 0, napi_default, 0 },
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
