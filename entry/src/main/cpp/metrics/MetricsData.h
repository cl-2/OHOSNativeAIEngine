// MetricsData.h - 性能监控数据结构定义
// 与业务逻辑完全隔离，独立头文件，可被任意模块包含
//
// 设计原则：
// 1. 通过 MetricType 枚举预留 0-31 为当前指标，32-63 预留，64+ 供未来扩展
// 2. 所有结构体 POD 类型，便于序列化
// 3. 不依赖任何业务头文件

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>

// ============================================================
// 指标类型枚举
// 设计说明：
// - 枚举值使用自然数顺序，新增指标只需在末尾追加，不影响已有值
// - 通过 unordered_map 存储，不依赖连续数组，天然支持稀疏扩展
// - 添加新指标时：在 Count 前追加一项即可，无需修改任何已有代码
// ============================================================
enum class MetricType : uint8_t {
    // === ASR 实时因子 ===
    AsrRtf = 0,                 // 实时因子 = 处理耗时 / 音频时长
    AsrDecodeLatencyMs = 1,     // 单次解码延迟(ms)

    // === VAD 指标 ===
    VadLatencyMs = 2,           // VAD 单次处理延迟(ms)
    VadSpeechDurationMs = 3,    // 检测到的语音段时长(ms)

    // === TTS 指标 ===
    TtsGenerationMs = 4,        // TTS 生成耗时(ms)
    TtsAudioLengthMs = 5,       // TTS 生成音频长度(ms)

    // === 缓冲区指标 ===
    RingBufferFillRate = 6,     // 环形缓冲区填充率 0.0~1.0
    AudioChunkSize = 7,         // 每次从 RingBuffer 读取的样本数

    // === LLM 指标 (从 ArkTS 侧采集) ===
    LlmLatencyMs = 8,           // LLM 响应延迟(ms)

    // === 预留：后续新增直接在这里加，不影响已有值 ===
    // JankFrameDuration = 9,
    // CpuUsagePercent = 10,
    // MemoryUsageMb = 11,
    // AudioPipelineLatencyMs = 12,
    // WakeWordLatencyMs = 13,
    // FullDuplexStateTransition = 14,

    Count = 9                   // 当前定义的指标总数（也是下一个新指标的 ID）
};

// ============================================================
// 时序数据点
// ============================================================
struct MetricPoint {
    int64_t timestamp;  // Unix 时间戳 (毫秒)
    double value;       // 指标值

    MetricPoint() : timestamp(0), value(0.0) {}
    MetricPoint(int64_t ts, double v) : timestamp(ts), value(v) {}
};

// ============================================================
// 统计摘要
// ============================================================
struct MetricSummary {
    double avg = 0.0;       // 均值
    double max = 0.0;       // 最大值
    double min = 0.0;       // 最小值
    double latest = 0.0;    // 最新值
    uint32_t count = 0;     // 样本数
    double p50 = 0.0;       // 50 分位（中位数）
    double p95 = 0.0;       // 95 分位
    double p99 = 0.0;       // 99 分位
};

// ============================================================
// 持久化快照
// ============================================================
struct MetricSnapshot {
    MetricType type;
    std::vector<MetricPoint> points;
};

// ============================================================
// 导出 JSON 辅助
// ============================================================
inline std::string MetricTypeToString(MetricType type) {
    switch (type) {
        case MetricType::AsrRtf:                return "asr_rtf";
        case MetricType::AsrDecodeLatencyMs:    return "asr_decode_latency_ms";
        case MetricType::VadLatencyMs:          return "vad_latency_ms";
        case MetricType::VadSpeechDurationMs:   return "vad_speech_duration_ms";
        case MetricType::TtsGenerationMs:       return "tts_generation_ms";
        case MetricType::TtsAudioLengthMs:      return "tts_audio_length_ms";
        case MetricType::RingBufferFillRate:    return "ring_buffer_fill_rate";
        case MetricType::AudioChunkSize:        return "audio_chunk_size";
        case MetricType::LlmLatencyMs:          return "llm_latency_ms";
        default:                                return "unknown";
    }
}
