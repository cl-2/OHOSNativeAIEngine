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

    // === 音频管线 ===
    AudioPipelineTotalMs = 9,   // FeedAudio 总耗时(ms)
    DcFilterLatencyUs = 10,     // DC 阻塞滤波器耗时(微秒)
    NoiseSuppressLatencyUs = 11, // 噪声抑制耗时(微秒)
    AecLatencyUs = 12,          // AEC 处理耗时(微秒)

    // === 端到端关键体验与资源 ===
    LlmFirstTokenMs = 13,       // LLM 首 Token 延迟(ms)
    LlmTokensPerSec = 14,       // LLM 平均生成速度(token/s)
    TtsFirstChunkMs = 15,       // TTS 首音频块延迟(ms)
    TtsRtf = 16,                // TTS 生成耗时 / 音频时长
    TtsQueueMs = 17,            // TTS 待播放 PCM 时长(ms)
    ProcessRssMb = 18,          // 当前进程 RSS(MB)
    ProcessPeakMb = 19,         // 当前进程历史峰值 RSS(MB)
    TtsThreadCount = 20,        // 当前 TTS 推理线程数

    LlmPromptTokens = 21,       // 本轮 prompt token 数
    LlmGeneratedTokens = 22,    // 本轮实际生成 token 数
    LlmPrefillMs = 23,          // prompt prefill 耗时(ms)
    LlmDecodeMs = 24,           // 生成阶段总耗时(ms)
    LlmBackendSelectMs = 25,    // HWCAP 检测与候选后端选择耗时(ms)
    LlmBackendLoadMs = 26,      // 目标 CPU backend 动态库加载耗时(ms)
    LlmModelLoadMs = 27,        // GGUF 模型映射与元数据加载耗时(ms)
    LlmContextInitMs = 28,      // llama context / KV cache 初始化耗时(ms)
    CpuDotprod = 29,            // 当前 CPU 是否支持 DotProd (0/1)
    CpuFp16 = 30,               // 当前 CPU 是否支持 FP16 vector arithmetic (0/1)
    LlmBackendTier = 31,        // 0=ARMv8, 1=DotProd, 2=DotProd+FP16
    AsrOrphanPartialReset = 32, // 未形成有效 VAD 语音段的 partial 清理事件
    AsrFillerDiscard = 33,      // 纯语气词 final/endpoint 丢弃事件
    AsrDecoderReset = 34,       // 由 ASR 解码线程串行执行的 stream reset
    Count = 35                  // 当前定义的指标总数
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
        case MetricType::AudioPipelineTotalMs:  return "audio_pipeline_total_ms";
        case MetricType::DcFilterLatencyUs:     return "dc_filter_latency_us";
        case MetricType::NoiseSuppressLatencyUs:return "noise_suppress_latency_us";
        case MetricType::AecLatencyUs:          return "aec_latency_us";
        case MetricType::LlmFirstTokenMs:       return "llm_first_token_ms";
        case MetricType::LlmTokensPerSec:       return "llm_tokens_per_sec";
        case MetricType::TtsFirstChunkMs:       return "tts_first_chunk_ms";
        case MetricType::TtsRtf:                return "tts_rtf";
        case MetricType::TtsQueueMs:            return "tts_queue_ms";
        case MetricType::ProcessRssMb:          return "process_rss_mb";
        case MetricType::ProcessPeakMb:         return "process_peak_mb";
        case MetricType::TtsThreadCount:        return "tts_thread_count";
        case MetricType::LlmPromptTokens:       return "llm_prompt_tokens";
        case MetricType::LlmGeneratedTokens:    return "llm_generated_tokens";
        case MetricType::LlmPrefillMs:          return "llm_prefill_ms";
        case MetricType::LlmDecodeMs:           return "llm_decode_ms";
        case MetricType::LlmBackendSelectMs:    return "llm_backend_select_ms";
        case MetricType::LlmBackendLoadMs:      return "llm_backend_load_ms";
        case MetricType::LlmModelLoadMs:        return "llm_model_load_ms";
        case MetricType::LlmContextInitMs:      return "llm_context_init_ms";
        case MetricType::CpuDotprod:             return "cpu_dotprod";
        case MetricType::CpuFp16:                return "cpu_fp16";
        case MetricType::LlmBackendTier:         return "llm_backend_tier";
        case MetricType::AsrOrphanPartialReset:  return "asr_orphan_partial_reset";
        case MetricType::AsrFillerDiscard:       return "asr_filler_discard";
        case MetricType::AsrDecoderReset:        return "asr_decoder_reset";
        default:                                return "unknown";
    }
}
