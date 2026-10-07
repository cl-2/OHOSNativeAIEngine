#ifndef PIPELINE_TYPES_H
#define PIPELINE_TYPES_H

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <cstdint>

/**
 * @brief 全双工音频管线事件类型
 *
 * 替代原有的字符串 action dispatch，用类型化事件路由。
 * 每个事件携带结构化载荷，编译期类型安全。
 *
 * 使用方式：
 *   PipelineDispatcher dispatcher;
 *   dispatcher.Register(PipelineEvent::Type::kLlmRequest,
 *       [](const PipelineEvent& e) { ... });
 *   dispatcher.Dispatch({PipelineEvent::Type::kLlmRequest, "你好"});
 */

struct PipelineEvent {
    enum Type : uint8_t {
        // VAD / 音频事件
        kAudioIn,
        kVadSpeechStart,
        kVadSpeechEnd,

        // ASR 事件
        kAsrInterim,
        kAsrFinal,

        // LLM 事件
        kLlmRequest,
        kLlmStart,
        kLlmComplete,

        // TTS 事件
        kTtsStart,
        kTtsComplete,

        // 打断 / 中断
        kInterrupted,
        kStopTts,
        kDuckTts,
        kResumeTts,

        // 全双工重叠模式
        kOverlapStart,      // 用户打断 AI 说话，进入重叠模式
        kOverlapEnd,        // 用户说完，退出重叠模式
    };

    Type type;
    std::string payload;

    PipelineEvent() : type(kInterrupted) {}
    PipelineEvent(Type t, const std::string& p = "") : type(t), payload(p) {}
};

/// 事件类型 → 可读字符串（调试用）
inline const char* PipelineEventTypeToString(PipelineEvent::Type type) {
    switch (type) {
        case PipelineEvent::kAudioIn:            return "audio_in";
        case PipelineEvent::kVadSpeechStart:     return "vad_start";
        case PipelineEvent::kVadSpeechEnd:       return "vad_end";
        case PipelineEvent::kAsrInterim:         return "asr_interim";
        case PipelineEvent::kAsrFinal:           return "asr_final";
        case PipelineEvent::kLlmRequest:         return "llm_request";
        case PipelineEvent::kLlmStart:           return "llm_start";
        case PipelineEvent::kLlmComplete:        return "llm_complete";
        case PipelineEvent::kTtsStart:           return "tts_started";
        case PipelineEvent::kTtsComplete:        return "tts_complete";
        case PipelineEvent::kInterrupted:        return "interrupted";
        case PipelineEvent::kStopTts:            return "stop_tts";
        case PipelineEvent::kDuckTts:            return "duck_tts";
        case PipelineEvent::kResumeTts:          return "resume_tts";
        case PipelineEvent::kOverlapStart:       return "overlap_start";
        case PipelineEvent::kOverlapEnd:         return "overlap_end";
        default:                                 return "unknown";
    }
}

/**
 * @brief 事件分发器
 *
 * 注册 → 分发模式。支持多个 handler 监听同一事件。
 * 线程安全由调用方保证（当前全在状态机线程使用）。
 */
class PipelineDispatcher {
public:
    using Handler = std::function<void(const PipelineEvent&)>;

    /// 注册事件处理器
    void Register(PipelineEvent::Type type, Handler handler) {
        m_handlers[static_cast<uint8_t>(type)].push_back(std::move(handler));
    }

    /// 分发事件到所有注册的处理器
    void Dispatch(const PipelineEvent& event) {
        auto it = m_handlers.find(static_cast<uint8_t>(event.type));
        if (it != m_handlers.end()) {
            for (auto& handler : it->second) {
                if (handler) handler(event);
            }
        }
    }

    /// 清空所有 handler
    void Clear() { m_handlers.clear(); }

private:
    std::unordered_map<uint8_t, std::vector<Handler>> m_handlers;
};

#endif // PIPELINE_TYPES_H
