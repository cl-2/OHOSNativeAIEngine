// MetricsCollector.h - 性能指标采集器
// 与业务逻辑完全隔离的埋点系统
//
// 使用方式：
//   MetricsCollector::Record(MetricType::AsrRtf, 0.15);
//   MetricsCollector::RecordAsrRtf(0.15);  // 便捷方法
//
// 扩展方式（以"卡顿检测"为例）：
//   1. 在 MetricsData.h 的 MetricType 枚举中追加：
//        JankFrameDuration = 9,
//   2. 更新 Count = 10
//   3. 在 MetricsCollector.h 添加便捷方法：
//        static void RecordJankDuration(int64_t ms);
//   4. 在卡顿检测代码中调用：
//        MetricsCollector::RecordJankDuration(42);
//    无需修改任何已有代码

#pragma once
#include "MetricsData.h"
#include <deque>
#include <unordered_map>
#include <mutex>
#include <string>
#include <functional>
#include <atomic>

class MetricsCollector {
public:
    // ======================== 生命周期 ========================

    // 初始化，指定持久化路径
    static void Init(const std::string& storagePath);

    // 启用/禁用采集（禁用时 Record 为空操作，编译期可优化）
    static void SetEnabled(bool enabled);
    static bool IsEnabled();

    // ======================== 埋点接口 ========================

    // 通用埋点：记录一个时序数据点
    // 线程安全，可从任意线程调用
    static void Record(MetricType type, double value);

    // --- 便捷埋点方法 ---
    static void RecordAsrRtf(double rtf);
    static void RecordAsrDecodeLatency(double ms);
    static void RecordVadLatency(double ms);
    static void RecordTtsGeneration(int64_t ms);
    static void RecordRingBufferFillRate(double rate);
    static void RecordLlmLatency(int64_t ms);
    static void RecordAudioPipelineTotal(double ms);
    static void RecordDcFilterLatency(double us);
    static void RecordNoiseSuppressLatency(double us);
    static void RecordAecLatency(double us);
    static void RecordLlmFirstToken(double ms);
    static void RecordLlmTokensPerSec(double tokensPerSec);
    static void RecordTtsFirstChunk(double ms);
    static void RecordTtsRtf(double rtf);
    static void RecordTtsQueueMs(double ms);
    static void RecordProcessMemory(double rssMb, double peakMb);
    static void RecordProcessMemorySnapshot();

    // ======================== 查询接口 ========================

    // 获取时序数据（sinceMs=0 返回全部）
    static std::vector<MetricPoint> GetTimeSeries(MetricType type, int64_t sinceMs = 0);

    // 获取统计摘要
    static MetricSummary GetSummary(MetricType type);

    // 获取最新 N 个数据点（用于折线图展示）
    static std::vector<MetricPoint> GetLatestPoints(MetricType type, size_t count);

    // 获取最新值
    static double GetLatest(MetricType type);

    // ======================== 管理接口 ========================

    // 重置所有指标
    static void Reset();

    // 重置单个指标
    static void ResetType(MetricType type);

    // 立即持久化到磁盘
    static void SaveToDisk();

    // 从磁盘加载历史数据
    static void LoadFromDisk();

    // 导出全部指标为 JSON 字符串
    static std::string ExportJson();

    // ======================== 扩展接口 ========================

    // 注册实时回调（供未来扩展：实时告警、卡顿检测推送等）
    // 每次 Record 时都会调用此回调
    using MetricCallback = std::function<void(MetricType type, double value, int64_t timestamp)>;
    static void SetMetricCallback(MetricCallback cb);

    // ======================== 常量 ========================

    // 每类指标最大保留点数（防止内存暴涨）
    static constexpr size_t kMaxPointsPerType = 10000;

    // 自动持久化间隔 (ms)
    static constexpr int64_t kSaveIntervalMs = 30000;

private:
    static std::string m_storagePath;
    static std::mutex m_mutex;
    static std::unordered_map<uint8_t, std::deque<MetricPoint>> m_series;
    static std::atomic<bool> m_enabled;
    static std::atomic<int64_t> m_lastSaveTime;
    static MetricCallback m_callback;

    // 内部：检查是否需要自动持久化
    static void CheckAutoSave();
};
