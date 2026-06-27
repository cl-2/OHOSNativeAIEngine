#ifndef CODEC_BENCHMARK_H
#define CODEC_BENCHMARK_H

#include "napi/native_api.h"

/**
 * @brief 硬件编解码器基准测试 NAPI 接口
 * 
 * 运行编解码性能测试，返回延迟、吞吐量等指标。
 * 用于展示硬件加速的优化效果对比。
 */

// 运行基准测试: (mimeType: string, testDurationMs: number) => { avgLatencyMs, throughputFps, ... }
napi_value RunCodecBenchmark(napi_env env, napi_callback_info info);

#endif // CODEC_BENCHMARK_H
