#include "BenchmarkEngine.h"
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cmath>

BenchmarkEngine::BenchmarkEngine() = default;

double BenchmarkEngine::ElapsedMs(TimePoint start, TimePoint end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
}

int64_t BenchmarkEngine::ReadProcStatus(const char* key) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;

    char line[256];
    int64_t value = -1;
    size_t keyLen = strlen(key);

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, keyLen) == 0) {
            // 格式: "VmRSS:  123456 kB"
            const char* p = line + keyLen;
            while (*p && (*p < '0' || *p > '9')) p++;
            if (*p) {
                value = atoll(p);
            }
            break;
        }
    }
    fclose(f);
    return value;
}

std::string BenchmarkEngine::CaptureMemorySnapshot() {
    std::ostringstream json;

    int64_t rss = ReadProcStatus("VmRSS:");
    int64_t peak = ReadProcStatus("VmHWM:");
    int64_t size = ReadProcStatus("VmSize:");
    int64_t threads = ReadProcStatus("Threads:");

    json << "\"memory\": {\n";
    json << "    \"rss_kb\": " << rss << ",\n";
    json << "    \"peak_kb\": " << peak << ",\n";
    json << "    \"size_kb\": " << size << ",\n";
    json << "    \"threads\": " << threads << ",\n";
    json << "    \"rss_mb\": " << (rss > 0 ? rss / 1024.0 : 0) << ",\n";
    json << "    \"peak_mb\": " << (peak > 0 ? peak / 1024.0 : 0) << "\n";
    json << "  }";

    return json.str();
}

std::string BenchmarkEngine::RunPipelineBenchmark(int iterations, int frameSize) {
    std::vector<double> latenciesMs;
    latenciesMs.reserve(iterations);

    // 构造模拟音频数据（静音 + 少许噪声）
    std::vector<float> dummyAudio(frameSize);
    for (int i = 0; i < frameSize; i++) {
        dummyAudio[i] = sinf((float)i * 0.01f) * 0.01f; // 极小噪声
    }

    // 预热：跑 10 次不记时
    for (int warmup = 0; warmup < 10; warmup++) {
        // 模拟 DC 滤波 + AEC 处理（只做运算，不依赖全局状态）
        float prevInput = 0, prevOutput = 0;
        for (int i = 0; i < frameSize; i++) {
            float y = dummyAudio[i] - prevInput + 0.95f * prevOutput;
            prevInput = dummyAudio[i];
            prevOutput = y;
        }
    }

    // 正式测试
    for (int iter = 0; iter < iterations; iter++) {
        auto start = Clock::now();

        // 模拟 DC 滤波（这是 FeedAudio 中的主要计算环节）
        float prevInput = 0, prevOutput = 0;
        for (int i = 0; i < frameSize; i++) {
            float y = dummyAudio[i] - prevInput + 0.95f * prevOutput;
            prevInput = dummyAudio[i];
            prevOutput = y;
        }

        auto end = Clock::now();
        latenciesMs.push_back(ElapsedMs(start, end));
    }

    // 统计
    double sum = 0, min = 1e9, max = 0;
    for (double v : latenciesMs) {
        sum += v;
        if (v < min) min = v;
        if (v > max) max = v;
    }
    double avg = sum / iterations;

    // 排序算分位
    std::vector<double> sorted = latenciesMs;
    std::sort(sorted.begin(), sorted.end());
    double p50 = sorted[iterations * 50 / 100];
    double p95 = sorted[iterations * 95 / 100];
    double p99 = sorted[iterations * 99 / 100];

    // 构造 JSON
    std::ostringstream json;
    json << "\"pipeline_benchmark\": {\n";
    json << "    \"iterations\": " << iterations << ",\n";
    json << "    \"frame_size\": " << frameSize << ",\n";
    json << "    \"latency_ms\": {\n";
    json << "      \"avg\": " << avg << ",\n";
    json << "      \"min\": " << min << ",\n";
    json << "      \"max\": " << max << ",\n";
    json << "      \"p50\": " << p50 << ",\n";
    json << "      \"p95\": " << p95 << ",\n";
    json << "      \"p99\": " << p99 << "\n";
    json << "    },\n";
    json << "    \"throughput\": {\n";
    json << "      \"frames_per_sec\": " << (avg > 0 ? 1000.0 / avg : 0) << ",\n";
    json << "      \"ms_per_frame\": " << avg << "\n";
    json << "    }\n";
    json << "  }";

    return json.str();
}

std::string BenchmarkEngine::RunAll() {
    std::ostringstream json;

    json << "{\n";

    // 1. 内存快照
    json << "  " << CaptureMemorySnapshot() << ",\n";

    // 2. Pipeline 压测
    json << "  " << RunPipelineBenchmark(1000, 5120) << ",\n";

    // 3. 汇总
    int64_t rss = ReadProcStatus("VmRSS:");
    json << "  \"summary\": {\n";
    json << "    \"status\": \"ok\",\n";
    json << "    \"rss_mb\": " << (rss > 0 ? rss / 1024.0 : 0) << "\n";
    json << "  }\n";

    json << "}\n";
    return json.str();
}
