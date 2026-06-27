#ifndef AUDIO_STATE_MACHINE_H
#define AUDIO_STATE_MACHINE_H

#include <atomic>
#include <functional>
#include <string>
#include <chrono>
#include <thread>
#include <condition_variable>

/**
 * @brief 全双工音频状态机 — 重构版
 * 
 * 职责：
 *   - 管理全双工会话的状态转换
 *   - 打断检测（RMS 能量阈值）
 *   - 超时看门狗（LLM 超时、ASR 超时）
 *   - 通过回调通知外部执行动作（LLM 请求、TTS 播放等）
 * 
 * 原则：
 *   - 状态机管控流程，外部只报告事件和执行动作
 *   - 所有状态转换由 TransitionTo 统一处理（日志+回调）
 *   - ArkTS 只负责 UI 渲染和 IO（HTTP/TTS 播放）
 */

enum class AudioState {
    IDLE,               // 空闲
    LISTENING,          // 录音中 / VAD 等待
    ASR_PROCESSING,     // VAD 结束 → ASR 推理中
    LLM_WAITING,        // 等待 LLM 响应
    TTS_PLAYING,        // TTS 播放中
    INTERRUPTED         // 打断过渡态（自动回到 LISTENING）
};

inline const char* AudioStateToString(AudioState state) {
    switch (state) {
        case AudioState::IDLE:           return "IDLE";
        case AudioState::LISTENING:      return "LISTENING";
        case AudioState::ASR_PROCESSING: return "ASR_PROCESSING";
        case AudioState::LLM_WAITING:    return "LLM_WAITING";
        case AudioState::TTS_PLAYING:    return "TTS_PLAYING";
        case AudioState::INTERRUPTED:    return "INTERRUPTED";
        default: return "UNKNOWN";
    }
}

// ========== 配置 ==========

struct AudioStateMachineConfig {
    float interruptThreshold = 0.02f;   // RMS 打断能量阈值
    int interruptMinFrames = 3;          // 连续超过阈值的检测帧数
    int interruptCheckInterval = 10;     // 每 N 次 FeedAudio 检查一次
    int llmTimeoutMs = 30000;            // LLM 等待超时
    int asrTimeoutMs = 15000;            // ASR 等待超时
    int interruptedAutoMs = 500;         // INTERRUPTED 停留后自动回 LISTENING
    bool debugLog = true;
};

// ========== 回调类型 ==========

/// 动作回调 — 状态机请求外部执行操作
//  action: "llm_request" | "tts_request" | "stop_tts" | "interrupted" | "asr_interim"
//  payload: 动作相关文本数据
using OnActionRequest = std::function<void(const std::string& action, const std::string& payload)>;

/// 状态变更回调
using OnStateChanged = std::function<void(AudioState oldState, AudioState newState)>;

// ============================================================
// AudioStateMachine
// ============================================================

class AudioStateMachine {
public:
    AudioStateMachine();
    ~AudioStateMachine();

    AudioStateMachine(const AudioStateMachine&) = delete;
    AudioStateMachine& operator=(const AudioStateMachine&) = delete;

    // ========== 生命周期 ==========

    /// 初始化
    void Init(const AudioStateMachineConfig& config);

    /// 启动 → LISTENING
    void Start();

    /// 停止 → IDLE（清看门狗、清状态）
    void Stop();

    /// 完全重置
    void Reset();

    bool IsRunning() const { return m_running.load(); }

    // ========== 查询 ==========

    AudioState GetCurrentState() const { return m_currentState.load(); }
    int64_t GetStateDurationMs() const;
    bool IsTtsPlaying() const { return m_currentState.load() == AudioState::TTS_PLAYING; }
    bool IsListening() const { return m_currentState.load() == AudioState::LISTENING; }

    // ========== 事件（由外部调用） ==========

    /// 音频输入 → 打断检测（由 FeedAudio 调用）
    void FeedAudio(const float* samples, size_t n);

    /// VAD 检测到语音开始（由 BackgroundAsrThread 调用）
    void OnVadSpeechStart();

    /// VAD 检测到语音结束（由 BackgroundAsrThread 调用）
    void OnVadSpeechEnd();

    /// ASR 返回中间结果（由 BackgroundAsrThread 调用）
    void OnAsrInterim(const std::string& text);

    /// ASR 返回最终结果 → 状态机决策进入 LLM_WAITING
    void OnAsrFinal(const std::string& text);

    /// LLM 开始流式返回（由 ArkTS 调用）
    void OnLlmStart();

    /// LLM 返回完成（由 ArkTS 调用）
    void OnLlmComplete(const std::string& fullText);

    /// TTS 播放开始（由 ArkTS 调用）
    void OnTtsStarted();

    /// TTS 播放完成（由 ArkTS 调用）
    void OnTtsComplete();

    /// 手动打断（由 ArkTS 调用）
    void Interrupt();

    // ========== 回调注册 ==========

    /// 注册动作请求回调（状态机 → ArkTS，通过 TSFN）
    void SetOnActionRequest(OnActionRequest cb) { m_onActionRequest = cb; }

    /// 注册状态变更回调
    void SetOnStateChanged(OnStateChanged cb) { m_onStateChanged = cb; }

private:
    // ========== 内部 ==========

    /// 核心状态转换
    void TransitionTo(AudioState newState);

    /// RMS 能量计算
    static float ComputeRms(const float* samples, size_t n);

    /// 触发动作回调
    void FireAction(const std::string& action, const std::string& payload = "");

    /// 看门狗线程
    void WatchdogLoop();

    /// 内部打断处理（清 RingBuffer + FireAction + 状态转换）
    void DoInterrupt();

    // ========== 状态 ==========

    std::atomic<AudioState> m_currentState{AudioState::IDLE};
    std::atomic<bool> m_running{false};
    AudioStateMachineConfig m_config;

    // 状态进入时间
    std::chrono::steady_clock::time_point m_stateStartTime;

    // 打断检测
    float m_rmsAccumulator = 0;
    int m_rmsCount = 0;
    int m_consecutiveInterruptFrames = 0;

    // ========== 看门狗 ==========

    std::thread m_watchdogThread;
    std::atomic<bool> m_watchdogStop{false};
    std::mutex m_watchdogMutex;
    std::condition_variable m_watchdogCV;

    // ========== 回调 ==========

    OnActionRequest m_onActionRequest;
    OnStateChanged m_onStateChanged;

    // ========== 日志 ==========

    void LogState(const char* event);
};

#endif // AUDIO_STATE_MACHINE_H
