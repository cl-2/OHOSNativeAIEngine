#include "AudioStateMachine.h"
#include <cmath>
#include <sstream>
#include <hilog/log.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "AudioStateMachine"
#define LOG_DOMAIN 0x0500

// ============================================================
// AudioStateMachine — 重构版
// 
// 状态转换表：
// ┌──────────────┬──────────────────────┬──────────────────┬─────────────────────────────┐
// │ 当前状态      │ 事件                 │ 下一状态          │ 动作                        │
// ├──────────────┼──────────────────────┼──────────────────┼─────────────────────────────┤
// │ IDLE         │ Start()              │ LISTENING        │ -                           │
// │ LISTENING    │ OnVadSpeechEnd()     │ ASR_PROCESSING   │ -                           │
// │ ASR_PROCESS  │ OnAsrInterim()       │ ASR_PROCESSING   │ Fire "asr_interim"          │
// │ ASR_PROCESS  │ OnAsrFinal(text)     │ LLM_WAITING      │ Fire "llm_request"(text)    │
// │ ASR_PROCESS  │ 超时(asrTimeoutMs)   │ LISTENING        │ -                           │
// │ LLM_WAITING  │ OnLlmComplete(text)  │ TTS_PLAYING      │ Fire "tts_request"(text)    │
// │ LLM_WAITING  │ 超时(llmTimeoutMs)   │ LISTENING        │ -                           │
// │ TTS_PLAYING  │ OnTtsComplete()      │ LISTENING        │ -                           │
// │ TTS_PLAYING  │ FeedAudio(打断)      │ INTERRUPTED      │ Fire "stop_tts" + "interrupted"
// │ TTS_PLAYING  │ OnVadSpeechStart()   │ INTERRUPTED      │ Fire "stop_tts" + "interrupted"
// │ INTERRUPTED  │ 自动(interruptedAutoMs)│ LISTENING      │ -                           │
// │ 任意         │ Stop()               │ IDLE             │ 停止看门狗                   │
// │ 任意         │ Interrupt()          │ INTERRUPTED      │ Fire "stop_tts" + "interrupted"
// └──────────────┴──────────────────────┴──────────────────┴─────────────────────────────┘
// ============================================================

constexpr const char* LOG_TAG_ASM = "AudioStateMachine";

AudioStateMachine::AudioStateMachine() {
}

AudioStateMachine::~AudioStateMachine() {
    Stop();
}

void AudioStateMachine::Init(const AudioStateMachineConfig& config) {
    m_config = config;
    m_currentState.store(AudioState::IDLE);
    m_running.store(false);
    m_consecutiveInterruptFrames = 0;
    m_rmsAccumulator = 0;
    m_rmsCount = 0;
    LogState("Init");
}

void AudioStateMachine::Start() {
    LogState("Start");
    m_running.store(true);
    // 启动看门狗线程
    m_watchdogStop.store(false);
    m_watchdogThread = std::thread(&AudioStateMachine::WatchdogLoop, this);
    TransitionTo(AudioState::LISTENING);
}

void AudioStateMachine::Stop() {
    LogState("Stop");
    m_running.store(false);
    // 停止看门狗
    {
        std::lock_guard<std::mutex> lock(m_watchdogMutex);
        m_watchdogStop.store(true);
    }
    m_watchdogCV.notify_one();
    if (m_watchdogThread.joinable()) {
        m_watchdogThread.join();
    }
    TransitionTo(AudioState::IDLE);
    m_consecutiveInterruptFrames = 0;
    m_rmsAccumulator = 0;
    m_rmsCount = 0;
}

void AudioStateMachine::Reset() {
    LogState("Reset");
    Stop();
    // Start 会在外部重新调用
}

int64_t AudioStateMachine::GetStateDurationMs() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_stateStartTime).count();
}

// ============================================================
// 音频输入 & 打断检测
// ============================================================

void AudioStateMachine::FeedAudio(const float* samples, size_t n) {
    if (!m_running.load()) return;

    // 只在 TTS_PLAYING 时进行打断检测
    if (m_currentState.load() != AudioState::TTS_PLAYING) {
        return;
    }

    float rms = ComputeRms(samples, n);
    m_rmsAccumulator += rms;
    m_rmsCount++;

    if (m_rmsCount >= m_config.interruptCheckInterval) {
        float avgRms = m_rmsAccumulator / m_rmsCount;
        m_rmsAccumulator = 0;
        m_rmsCount = 0;

        if (avgRms > m_config.interruptThreshold) {
            m_consecutiveInterruptFrames++;
            OH_LOG_INFO(LOG_APP, "%{public}s: interrupt candidate #%{public}d, rms=%.4f",
                       LOG_TAG_ASM, m_consecutiveInterruptFrames, avgRms);
        } else if (m_consecutiveInterruptFrames > 0) {
            m_consecutiveInterruptFrames--;
        }

        if (m_consecutiveInterruptFrames >= m_config.interruptMinFrames) {
            LogState("interrupt triggered by RMS");
            m_consecutiveInterruptFrames = 0;
            DoInterrupt();
        }
    }
}

// ============================================================
// 事件处理
// ============================================================

void AudioStateMachine::OnVadSpeechStart() {
    LogState("VAD speech START");
    if (m_currentState.load() == AudioState::TTS_PLAYING) {
        TransitionTo(AudioState::BARGE_IN_CANDIDATE);
        FireAction("duck_tts", "");
    }
}

bool AudioStateMachine::ConfirmBargeIn() {
    if (m_currentState.load() != AudioState::BARGE_IN_CANDIDATE) return false;
    DoInterrupt();
    return m_currentState.load() == AudioState::INTERRUPTED;
}

void AudioStateMachine::CancelBargeIn() {
    if (m_currentState.load() == AudioState::BARGE_IN_CANDIDATE) {
        FireAction("resume_tts", "");
        TransitionTo(AudioState::TTS_PLAYING);
    }
}

void AudioStateMachine::OnVadSpeechEnd() {
    LogState("VAD speech END");
    if (m_currentState.load() == AudioState::LISTENING) {
        TransitionTo(AudioState::ASR_PROCESSING);
    }
}

void AudioStateMachine::OnAsrInterim(const std::string& text) {
    if (m_config.debugLog) {
        OH_LOG_INFO(LOG_APP, "%{public}s: ASR interim: %{public}s", LOG_TAG_ASM, text.c_str());
    }
    // 通知 ArkTS 显示中间结果（不转换状态）
    FireAction("asr_interim", text);
    if (m_currentState.load() == AudioState::BARGE_IN_CANDIDATE && text.size() >= 2) {
        ConfirmBargeIn();
    }
}

void AudioStateMachine::OnAsrFinal(const std::string& text) {
    LogState(("ASR final: " + text).c_str());

    if (m_currentState.load() != AudioState::ASR_PROCESSING &&
        m_currentState.load() != AudioState::LISTENING) {
        // 如果不在正确处理状态，忽略
        OH_LOG_WARN(LOG_APP, "%{public}s: ignoring ASR final in state %{public}s",
                    LOG_TAG_ASM, AudioStateToString(m_currentState.load()));
        return;
    }

    // ASR 完成 → 请求 LLM
    TransitionTo(AudioState::LLM_WAITING);
    FireAction("llm_request", text);
}

void AudioStateMachine::OnLlmStart() {
    LogState("LLM start");
    // 确保在 LLM_WAITING 状态
    if (m_currentState.load() != AudioState::LLM_WAITING) {
        TransitionTo(AudioState::LLM_WAITING);
    }
}

void AudioStateMachine::OnLlmComplete(const std::string& fullText) {
    LogState("LLM complete");
    // TTS 可能已在流式过程中由 ArkTS 开始播放，接受 TTS_PLAYING 状态
    auto curState = m_currentState.load();
    if (curState != AudioState::LLM_WAITING && curState != AudioState::TTS_PLAYING) {
        OH_LOG_WARN(LOG_APP, "%{public}s: LLM complete but state=%{public}s",
                    LOG_TAG_ASM, AudioStateToString(curState));
        return;
    }
    if (!fullText.empty()) {
        // 仅在尚未进入 TTS_PLAYING 时转换
        if (curState != AudioState::TTS_PLAYING && m_running.load()) {
            TransitionTo(AudioState::TTS_PLAYING);
        }
    } else {
        // 空回复 → 直接回 LISTENING
        if (m_running.load()) {
            TransitionTo(AudioState::LISTENING);
        }
    }
}

void AudioStateMachine::OnTtsStarted() {
    LogState("TTS started");
    // 确认已在 TTS_PLAYING
    if (m_currentState.load() != AudioState::TTS_PLAYING) {
        TransitionTo(AudioState::TTS_PLAYING);
    }
}

void AudioStateMachine::OnTtsComplete() {
    LogState("TTS complete");
    if (m_running.load()) {
        TransitionTo(AudioState::LISTENING);
    }
}

void AudioStateMachine::OnBargeInTtsStopped() {
    LogState("Barge-in TTS stopped");
    if (m_running.load() && m_currentState.load() == AudioState::INTERRUPTED) {
        TransitionTo(AudioState::LISTENING);
    }
}

void AudioStateMachine::Interrupt() {
    LogState("Interrupt (manual)");
    if (m_currentState.load() == AudioState::IDLE) return;
    DoInterrupt();
}

// ============================================================
// 内部方法
// ============================================================

void AudioStateMachine::DoInterrupt() {
    auto current = m_currentState.load();
    if (current == AudioState::IDLE || current == AudioState::INTERRUPTED) return;

    // 停止 TTS + 通知打断
    FireAction("stop_tts", "");

    TransitionTo(AudioState::INTERRUPTED);
    FireAction("interrupted", "");

    // 重置打断计数
    m_consecutiveInterruptFrames = 0;
    m_rmsAccumulator = 0;
    m_rmsCount = 0;

    // INTERRUPTED 会自动过渡到 LISTENING（看门狗线程处理延迟）
    // 唤醒看门狗立即处理
    m_watchdogCV.notify_one();
}

void AudioStateMachine::TransitionTo(AudioState newState) {
    AudioState oldState = m_currentState.exchange(newState);
    if (oldState == newState) return;

    m_stateStartTime = std::chrono::steady_clock::now();

    if (m_config.debugLog) {
        OH_LOG_INFO(LOG_APP, "%{public}s: [%{public}s] → [%{public}s] (dur=%{public}lldms)",
                    LOG_TAG_ASM,
                    AudioStateToString(oldState), AudioStateToString(newState),
                    static_cast<long long>(oldState != AudioState::IDLE ? GetStateDurationMs() : 0LL));
    }

    // 唤醒看门狗（状态变了，超时时间要重新算）
    m_watchdogCV.notify_one();

    if (m_onStateChanged) {
        m_onStateChanged(oldState, newState);
    }
}

void AudioStateMachine::FireAction(const std::string& action, const std::string& payload) {
    if (m_onActionRequest) {
        m_onActionRequest(action, payload);
    } else {
        OH_LOG_WARN(LOG_APP, "%{public}s: no action callback registered for %{public}s",
                    LOG_TAG_ASM, action.c_str());
    }
}

float AudioStateMachine::ComputeRms(const float* samples, size_t n) {
    if (n == 0) return 0.0f;
    double sumSq = 0.0;
    for (size_t i = 0; i < n; i++) {
        sumSq += static_cast<double>(samples[i]) * samples[i];
    }
    return static_cast<float>(std::sqrt(sumSq / n));
}

void AudioStateMachine::LogState(const char* event) {
    if (m_config.debugLog) {
        OH_LOG_INFO(LOG_APP, "%{public}s: %{public}s [%{public}s]",
                    LOG_TAG_ASM, event, AudioStateToString(m_currentState.load()));
    }
}

// ============================================================
// 看门狗线程
// ============================================================

void AudioStateMachine::WatchdogLoop() {
    OH_LOG_INFO(LOG_APP, "%{public}s: watchdog started", LOG_TAG_ASM);

    while (m_running.load() && !m_watchdogStop.load()) {
        AudioState currentState = m_currentState.load();

        if (currentState == AudioState::INTERRUPTED) {
            // 等待 AudioRenderer 确认停止后再重新开放主 ASR。
            std::unique_lock<std::mutex> lock(m_watchdogMutex);
            m_watchdogCV.wait_for(lock, std::chrono::milliseconds(100),
                [this]() { return m_watchdogStop.load() || !m_running.load(); });
            continue;
        }

        if (currentState == AudioState::LLM_WAITING) {
            int64_t dur = GetStateDurationMs();
            if (dur >= m_config.llmTimeoutMs) {
                OH_LOG_WARN(LOG_APP, "%{public}s: LLM timeout after %{public}lldms", LOG_TAG_ASM, (long long)dur);
                if (m_running.load()) {
                    TransitionTo(AudioState::LISTENING);
                }
                continue;
            }
            int waitMs = m_config.llmTimeoutMs - static_cast<int>(dur);
            std::unique_lock<std::mutex> lock(m_watchdogMutex);
            m_watchdogCV.wait_for(lock, std::chrono::milliseconds(std::min(1000, waitMs)),
                [this]() { return m_watchdogStop.load() || !m_running.load(); });
            continue;
        }

        if (currentState == AudioState::ASR_PROCESSING) {
            int64_t dur = GetStateDurationMs();
            if (dur >= m_config.asrTimeoutMs) {
                OH_LOG_WARN(LOG_APP, "%{public}s: ASR timeout after %{public}lldms", LOG_TAG_ASM, (long long)dur);
                if (m_running.load()) {
                    TransitionTo(AudioState::LISTENING);
                }
                continue;
            }
            int waitMs = m_config.asrTimeoutMs - static_cast<int>(dur);
            std::unique_lock<std::mutex> lock(m_watchdogMutex);
            m_watchdogCV.wait_for(lock, std::chrono::milliseconds(std::min(1000, waitMs)),
                [this]() { return m_watchdogStop.load() || !m_running.load(); });
            continue;
        }

        // LISTENING 或 TTS_PLAYING：不需要超时，每秒唤醒一次检查即可
        {
            std::unique_lock<std::mutex> lock(m_watchdogMutex);
            m_watchdogCV.wait_for(lock, std::chrono::milliseconds(1000),
                [this]() { return m_watchdogStop.load() || !m_running.load(); });
        }
    }

    OH_LOG_INFO(LOG_APP, "%{public}s: watchdog stopped", LOG_TAG_ASM);
}
