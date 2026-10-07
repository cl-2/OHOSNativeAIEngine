#ifndef BENCHMARK_ENGINE_H
#define BENCHMARK_ENGINE_H

#include <string>
#include <vector>
#include <chrono>

/**
 * @brief 性能基准测试引擎
 *
 * 职责：
 *   1. 运行标准化测试流程（pipeline 压测）
 *   2. 采集内存快照（RSS / VmSize / Threads）
 *   3. 返回结构化 JSON 结果，支持 ArkTS 解析和对比
 *
 * 使用方式：
 *   BenchmarkEngine bench;
 *   std::string json = bench.RunAll();  // 一键压测
 *
 * 面试价值：
 *   "优化前先跑 benchmark 出基线，优化后再跑，对比数据说话"
 *   这是工程素养的体现，不是光靠感觉说我优化了。
 */

class BenchmarkEngine {
public:
    BenchmarkEngine();
    ~BenchmarkEngine() = default;

    // ========== 标准化测试 ==========

    /// 运行全部 benchmark，返回 JSON 字符串
    std::string RunAll();

    // ========== 单项测试 ==========

    /// 内存快照：返回 JSON 片段
    std::string CaptureMemorySnapshot();

    /// Pipeline 压测：模拟 N 次音频帧处理，返回耗时统计
    /// @param iterations 迭代次数（默认 1000）
    /// @param frameSize  每帧样本数（默认 5120 = 320ms @16kHz）
    std::string RunPipelineBenchmark(int iterations = 1000, int frameSize = 5120);

    // ========== 工具 ==========

    /// 从 /proc/self/status 读取内存信息
    /// @param key 如 "VmRSS", "VmSize", "Threads"
    /// @return 数值（kB），-1 表示读取失败
    static int64_t ReadProcStatus(const char* key);

private:
    /// 时间工具
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    static double ElapsedMs(TimePoint start, TimePoint end);
};

#endif // BENCHMARK_ENGINE_H
