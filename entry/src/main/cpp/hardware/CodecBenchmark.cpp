#include "CodecBenchmark.h"
#include "MediaCodecAdapter.h"
#include "IHardwareCodec.h"
#include "napi/native_api.h"
#include "hilog/log.h"
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>
#include <random>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "CodecBench"
#define LOG_DOMAIN 0x0300

/**
 * @brief 硬件编解码基准测试
 * 
 * 测试流程：
 * 1. 创建 MediaCodec 解码器实例
 * 2. 向解码器喂入模拟 H.264 数据，持续指定时长
 * 3. 收集解码延迟、吞吐量指标
 * 4. 输出 JSON 格式的测试报告
 */

// 生成模拟 H.264 帧数据（用于基准测试）
static std::vector<uint8_t> GenerateMockFrame(int frameNum, int width, int height) {
    // 生成模拟的 YUV420P 数据（不编码，直接用原始数据模拟解码负载）
    size_t ySize = width * height;
    size_t uvSize = (width / 2) * (height / 2);
    size_t totalSize = ySize + 2 * uvSize;
    
    std::vector<uint8_t> frame(totalSize);
    
    // 用伪随机但确定性的数据填充（保证不同帧有差异但可重复）
    std::mt19937 rng(frameNum * 12345 + 67890);
    for (size_t i = 0; i < totalSize; i++) {
        frame[i] = static_cast<uint8_t>(rng() & 0xFF);
    }
    
    return frame;
}

napi_value RunCodecBenchmark(napi_env env, napi_callback_info info) {
    OH_LOG_INFO(LOG_APP, "CodecBench: RunCodecBenchmark called");
    
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    
    // 解析参数
    size_t mimeLen = 0;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &mimeLen);
    std::string mimeType(mimeLen, '\0');
    napi_get_value_string_utf8(env, argv[0], &mimeType[0], mimeLen + 1, &mimeLen);
    mimeType.resize(mimeLen);
    
    int32_t testDurationMs = 5000; // 默认 5 秒
    if (argc >= 2) {
        napi_get_value_int32(env, argv[1], &testDurationMs);
    }
    
    OH_LOG_INFO(LOG_APP, "CodecBench: mime=%{public}s, duration=%{public}dms",
                mimeType.c_str(), testDurationMs);
    
    // 创建结果对象
    napi_value result;
    napi_create_object(env, &result);
    
    // ---- 运行基准测试 ----
    auto benchStart = std::chrono::steady_clock::now();
    
    // 1. 创建硬件编解码器
    auto codec = CreateHardwareCodec();
    if (!codec) {
        OH_LOG_ERROR(LOG_APP, "CodecBench: Failed to create codec instance");
        napi_value errorMsg;
        napi_create_string_utf8(env, "Failed to create hardware codec", NAPI_AUTO_LENGTH, &errorMsg);
        napi_set_named_property(env, result, "error", errorMsg);
        napi_get_boolean(env, false, &result);
        return result;
    }
    
    // 2. 初始化解码器
    bool initOk = codec->Init(mimeType, false);
    if (!initOk) {
        OH_LOG_ERROR(LOG_APP, "CodecBench: Failed to init codec");
        napi_value errorMsg;
        napi_create_string_utf8(env, "Failed to init hardware codec", NAPI_AUTO_LENGTH, &errorMsg);
        napi_set_named_property(env, result, "error", errorMsg);
        napi_get_boolean(env, false, &result);
        return result;
    }
    
    // 3. 基准测试循环
    const int width = 1280;
    const int height = 720;
    int totalFrames = 0;
    int decodedFrames = 0;
    std::vector<double> frameLatencies;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(testDurationMs);
    
    OH_LOG_INFO(LOG_APP, "CodecBench: Starting benchmark loop for %{public}dms", testDurationMs);
    
    while (std::chrono::steady_clock::now() < deadline) {
        auto frameStart = std::chrono::steady_clock::now();
        
        // 生成模拟帧
        auto mockData = GenerateMockFrame(totalFrames, width, height);
        int64_t pts = totalFrames * 33; // 模拟 30fps 的 PTS
        
        // 喂入解码器
        bool queueOk = codec->QueueInput(mockData.data(), mockData.size(), pts);
        if (!queueOk) {
            OH_LOG_WARN(LOG_APP, "CodecBench: QueueInput failed at frame %{public}d", totalFrames);
            break;
        }
        totalFrames++;
        
        // 尝试取解码输出（非阻塞）
        std::vector<uint8_t> outData;
        int64_t outPts = 0;
        bool dequeueOk = codec->DequeueOutput(outData, outPts);
        
        auto frameEnd = std::chrono::steady_clock::now();
        double latencyMs = std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart).count() / 1000.0;
        
        if (dequeueOk) {
            decodedFrames++;
            frameLatencies.push_back(latencyMs);
        }
        
        // 小休眠模拟真实帧间隔
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    
    auto benchEnd = std::chrono::steady_clock::now();
    double actualDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(benchEnd - benchStart).count();
    
    // 4. 清理
    codec->Release();
    
    // 5. 计算统计指标
    double avgLatency = 0, minLatency = 999999, maxLatency = 0;
    double p95Latency = 0, p99Latency = 0;
    
    if (!frameLatencies.empty()) {
        std::sort(frameLatencies.begin(), frameLatencies.end());
        for (auto v : frameLatencies) {
            avgLatency += v;
            if (v < minLatency) minLatency = v;
            if (v > maxLatency) maxLatency = v;
        }
        avgLatency /= frameLatencies.size();
        
        size_t idx95 = static_cast<size_t>(frameLatencies.size() * 0.95);
        size_t idx99 = static_cast<size_t>(frameLatencies.size() * 0.99);
        if (idx95 >= frameLatencies.size()) idx95 = frameLatencies.size() - 1;
        if (idx99 >= frameLatencies.size()) idx99 = frameLatencies.size() - 1;
        p95Latency = frameLatencies[idx95];
        p99Latency = frameLatencies[idx99];
    }
    
    double throughputFps = actualDurationMs > 0 ? (decodedFrames * 1000.0 / actualDurationMs) : 0;
    
    OH_LOG_INFO(LOG_APP, "CodecBench: Results: frames=%{public}d/%{public}d, avgLat=%.2fms, fps=%.1f",
                decodedFrames, totalFrames, avgLatency, throughputFps);
    
    // 6. 填充结果对象
    napi_value jsCodecName;
    napi_create_string_utf8(env, codec->GetName().c_str(), NAPI_AUTO_LENGTH, &jsCodecName);
    napi_set_named_property(env, result, "codecName", jsCodecName);
    
    napi_value jsMimeType;
    napi_create_string_utf8(env, mimeType.c_str(), NAPI_AUTO_LENGTH, &jsMimeType);
    napi_set_named_property(env, result, "mimeType", jsMimeType);
    
    napi_value jsDuration;
    napi_create_double(env, actualDurationMs, &jsDuration);
    napi_set_named_property(env, result, "testDurationMs", jsDuration);
    
    napi_value jsTotalFrames;
    napi_create_int32(env, totalFrames, &jsTotalFrames);
    napi_set_named_property(env, result, "totalFrames", jsTotalFrames);
    
    napi_value jsDecodedFrames;
    napi_create_int32(env, decodedFrames, &jsDecodedFrames);
    napi_set_named_property(env, result, "decodedFrames", jsDecodedFrames);
    
    napi_value jsThroughput;
    napi_create_double(env, throughputFps, &jsThroughput);
    napi_set_named_property(env, result, "throughputFps", jsThroughput);
    
    napi_value jsAvgLatency;
    napi_create_double(env, avgLatency, &jsAvgLatency);
    napi_set_named_property(env, result, "avgLatencyMs", jsAvgLatency);
    
    napi_value jsMinLatency;
    napi_create_double(env, minLatency, &jsMinLatency);
    napi_set_named_property(env, result, "minLatencyMs", jsMinLatency);
    
    napi_value jsMaxLatency;
    napi_create_double(env, maxLatency, &jsMaxLatency);
    napi_set_named_property(env, result, "maxLatencyMs", jsMaxLatency);
    
    napi_value jsP95Latency;
    napi_create_double(env, p95Latency, &jsP95Latency);
    napi_set_named_property(env, result, "p95LatencyMs", jsP95Latency);
    
    napi_value jsP99Latency;
    napi_create_double(env, p99Latency, &jsP99Latency);
    napi_set_named_property(env, result, "p99LatencyMs", jsP99Latency);
    
    OH_LOG_INFO(LOG_APP, "CodecBench: benchmark complete");
    return result;
}
