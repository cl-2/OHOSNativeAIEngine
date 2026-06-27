// MetricsCollector.cpp - 性能指标采集器实现
#include "MetricsCollector.h"
#include "hilog/log.h"
#include <fstream>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "OHOS_Metrics"

// ======================== 静态成员初始化 ========================
std::string MetricsCollector::m_storagePath;
std::mutex MetricsCollector::m_mutex;
std::unordered_map<uint8_t, std::deque<MetricPoint>> MetricsCollector::m_series;
bool MetricsCollector::m_enabled = false;
int64_t MetricsCollector::m_lastSaveTime = 0;
MetricsCollector::MetricCallback MetricsCollector::m_callback = nullptr;

// ======================== 生命周期 ========================

void MetricsCollector::Init(const std::string& storagePath) {
    m_storagePath = storagePath;
    OH_LOG_INFO(LOG_APP, "ohos_Metrics MetricsCollector initialized, storage=%{public}s",
                storagePath.c_str());

    // 尝试加载历史数据
    LoadFromDisk();
}

void MetricsCollector::SetEnabled(bool enabled) {
    m_enabled = enabled;
    if (enabled) {
        m_lastSaveTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    OH_LOG_INFO(LOG_APP, "ohos_Metrics %s", enabled ? "enabled" : "disabled");
}

bool MetricsCollector::IsEnabled() {
    return m_enabled;
}

// ======================== 埋点接口 ========================

void MetricsCollector::Record(MetricType type, double value) {
    if (!m_enabled) return;

    uint8_t typeId = static_cast<uint8_t>(type);
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& deque = m_series[typeId];
        deque.emplace_back(now, value);

        // 限制最大点数
        while (deque.size() > kMaxPointsPerType) {
            deque.pop_front();
        }
    }

    // 触发实时回调
    if (m_callback) {
        m_callback(type, value, now);
    }

    // 检查自动保存
    CheckAutoSave();
}

// ======================== 便捷埋点 ========================

void MetricsCollector::RecordAsrRtf(double rtf) {
    Record(MetricType::AsrRtf, rtf);
}

void MetricsCollector::RecordAsrDecodeLatency(int64_t ms) {
    Record(MetricType::AsrDecodeLatencyMs, static_cast<double>(ms));
}

void MetricsCollector::RecordVadLatency(int64_t ms) {
    Record(MetricType::VadLatencyMs, static_cast<double>(ms));
}

void MetricsCollector::RecordTtsGeneration(int64_t ms) {
    Record(MetricType::TtsGenerationMs, static_cast<double>(ms));
}

void MetricsCollector::RecordRingBufferFillRate(double rate) {
    Record(MetricType::RingBufferFillRate, rate);
}

void MetricsCollector::RecordLlmLatency(int64_t ms) {
    Record(MetricType::LlmLatencyMs, static_cast<double>(ms));
}

// ======================== 查询接口 ========================

std::vector<MetricPoint> MetricsCollector::GetTimeSeries(MetricType type, int64_t sinceMs) {
    uint8_t typeId = static_cast<uint8_t>(type);
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_series.find(typeId);
    if (it == m_series.end()) return {};

    const auto& deque = it->second;
    if (sinceMs <= 0) {
        return {deque.begin(), deque.end()};
    }

    std::vector<MetricPoint> result;
    for (const auto& pt : deque) {
        if (pt.timestamp >= sinceMs) {
            result.push_back(pt);
        }
    }
    return result;
}

MetricSummary MetricsCollector::GetSummary(MetricType type) {
    uint8_t typeId = static_cast<uint8_t>(type);
    MetricSummary summary;
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_series.find(typeId);
    if (it == m_series.end() || it->second.empty()) return summary;

    const auto& deque = it->second;
    summary.latest = deque.back().value;
    summary.count = static_cast<uint32_t>(deque.size());

    // 计算 min/max/avg
    double sum = 0.0;
    summary.min = deque.front().value;
    summary.max = deque.front().value;

    std::vector<double> values;
    values.reserve(deque.size());

    for (const auto& pt : deque) {
        double v = pt.value;
        sum += v;
        if (v < summary.min) summary.min = v;
        if (v > summary.max) summary.max = v;
        values.push_back(v);
    }
    summary.avg = sum / deque.size();

    // 计算分位数
    std::sort(values.begin(), values.end());
    size_t n = values.size();
    auto percentile = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p * (n - 1));
        return values[idx];
    };
    summary.p50 = percentile(0.50);
    summary.p95 = percentile(0.95);
    summary.p99 = percentile(0.99);

    return summary;
}

std::vector<MetricPoint> MetricsCollector::GetLatestPoints(MetricType type, size_t count) {
    uint8_t typeId = static_cast<uint8_t>(type);
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_series.find(typeId);
    if (it == m_series.end() || it->second.empty()) return {};

    const auto& deque = it->second;
    size_t start = (deque.size() > count) ? deque.size() - count : 0;
    auto begin = deque.begin();
    std::advance(begin, start);
    return {begin, deque.end()};
}

double MetricsCollector::GetLatest(MetricType type) {
    uint8_t typeId = static_cast<uint8_t>(type);
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_series.find(typeId);
    if (it == m_series.end() || it->second.empty()) return 0.0;
    return it->second.back().value;
}

// ======================== 管理接口 ========================

void MetricsCollector::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_series.clear();
    OH_LOG_INFO(LOG_APP, "ohos_Metrics all metrics reset");
}

void MetricsCollector::ResetType(MetricType type) {
    uint8_t typeId = static_cast<uint8_t>(type);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_series.erase(typeId);
}

void MetricsCollector::SaveToDisk() {
    if (m_storagePath.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    std::ofstream file(m_storagePath, std::ios::binary);
    if (!file.is_open()) {
        OH_LOG_ERROR(LOG_APP, "ohos_Metrics failed to open for save: %{public}s",
                     m_storagePath.c_str());
        return;
    }

    // 文件头: 魔数 + 版本
    const uint32_t magic = 0x4D545258;  // "MTRX"
    const uint32_t version = 1;
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));

    // 写入类型数
    uint32_t typeCount = static_cast<uint32_t>(m_series.size());
    file.write(reinterpret_cast<const char*>(&typeCount), sizeof(typeCount));

    // 逐类型写入
    for (auto it = m_series.begin(); it != m_series.end(); ++it) {
        uint8_t typeId = it->first;
        const auto& deque = it->second;
        // 类型 ID
        file.write(reinterpret_cast<const char*>(&typeId), sizeof(typeId));

        // 点数
        uint32_t pointCount = static_cast<uint32_t>(deque.size());
        file.write(reinterpret_cast<const char*>(&pointCount), sizeof(pointCount));

        // 数据点
        for (const auto& pt : deque) {
            file.write(reinterpret_cast<const char*>(&pt.timestamp), sizeof(pt.timestamp));
            file.write(reinterpret_cast<const char*>(&pt.value), sizeof(pt.value));
        }
    }

    file.close();
    OH_LOG_INFO(LOG_APP, "ohos_Metrics saved %{public}u metric types to %{public}s",
                typeCount, m_storagePath.c_str());
}

void MetricsCollector::LoadFromDisk() {
    if (m_storagePath.empty()) return;

    std::ifstream file(m_storagePath, std::ios::binary);
    if (!file.is_open()) {
        OH_LOG_INFO(LOG_APP, "ohos_Metrics no existing data file (first run)");
        return;
    }

    // 验证魔数和版本
    uint32_t magic = 0, version = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (magic != 0x4D545258 || version != 1) {
        OH_LOG_WARN(LOG_APP, "ohos_Metrics invalid data file (magic=0x%08x, ver=%u)", magic, version);
        return;
    }

    uint32_t typeCount = 0;
    file.read(reinterpret_cast<char*>(&typeCount), sizeof(typeCount));

    std::lock_guard<std::mutex> lock(m_mutex);
    size_t totalPoints = 0;

    for (uint32_t t = 0; t < typeCount; t++) {
        uint8_t typeId = 0;
        file.read(reinterpret_cast<char*>(&typeId), sizeof(typeId));

        uint32_t pointCount = 0;
        file.read(reinterpret_cast<char*>(&pointCount), sizeof(pointCount));

        auto& deque = m_series[typeId];
        for (uint32_t p = 0; p < pointCount; p++) {
            int64_t ts = 0;
            double val = 0.0;
            file.read(reinterpret_cast<char*>(&ts), sizeof(ts));
            file.read(reinterpret_cast<char*>(&val), sizeof(val));
            deque.emplace_back(ts, val);
        }
        totalPoints += pointCount;
    }

    file.close();
    OH_LOG_INFO(LOG_APP, "ohos_Metrics loaded %{public}u types, %{public}zu points from %{public}s",
                typeCount, totalPoints, m_storagePath.c_str());
}

std::string MetricsCollector::ExportJson() {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ostringstream json;
    json << "{\n";
    bool firstType = true;

    for (auto it = m_series.begin(); it != m_series.end(); ++it) {
        uint8_t typeId = it->first;
        const auto& deque = it->second;
        if (!firstType) json << ",\n";
        firstType = false;

        MetricType type = static_cast<MetricType>(typeId);
        json << "  \"" << MetricTypeToString(type) << "\": {\n";
        json << "    \"count\": " << deque.size() << ",\n";

        // 统计
        double sum = 0, minVal = 0, maxVal = 0;
        if (!deque.empty()) {
            minVal = maxVal = deque.front().value;
            for (const auto& pt : deque) {
                sum += pt.value;
                if (pt.value < minVal) minVal = pt.value;
                if (pt.value > maxVal) maxVal = pt.value;
            }
            json << "    \"avg\": " << (sum / deque.size()) << ",\n";
            json << "    \"min\": " << minVal << ",\n";
            json << "    \"max\": " << maxVal << ",\n";
            json << "    \"latest\": " << deque.back().value << ",\n";
        }

        // 时序数据（前 3 个 + 后 3 个，避免 JSON 过大）
        json << "    \"points\": [\n";
        size_t n = deque.size();
        size_t showCount = std::min(n, size_t(6));
        auto dit = deque.begin();
        for (size_t i = 0; i < showCount; i++) {
            if (i > 0 && n > 6 && i == 3) {
                // 跳过中间部分
                std::advance(dit, n - 6);
                i = 3;
            }
            if (i > 0) json << ",\n";
            json << "      { \"t\": " << dit->timestamp << ", \"v\": " << dit->value << " }";
            ++dit;
        }
        if (n > 6) {
            json << ",\n      ... truncated " << (n - 6) << " points ...";
        }
        json << "\n    ]\n  }";
    }

    json << "\n}\n";
    return json.str();
}

// ======================== 扩展接口 ========================

void MetricsCollector::SetMetricCallback(MetricCallback cb) {
    m_callback = std::move(cb);
}

// ======================== 内部方法 ========================

void MetricsCollector::CheckAutoSave() {
    if (m_storagePath.empty()) return;

    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (now - m_lastSaveTime >= kSaveIntervalMs) {
        m_lastSaveTime = now;
        // 异步保存，不阻塞调用线程
        std::thread(SaveToDisk).detach();
    }
}
