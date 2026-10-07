#include "NoiseSuppressor.h"
#include <hilog/log.h>
#include <dlfcn.h>
#include <cstring>
#include <cmath>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0500
#define LOG_TAG "NoiseSuppressor"

// ============================================================
// RNNoise 动态加载适配层
//
// RNNoise 库通过 dlopen 在运行时加载，避免编译时依赖。
// 如果 librnnoise.so 不可用，噪声抑制自动跳过（安全降级）。
//
// RNNoise 是可选的运行时本地依赖，需由部署环境提供目标架构的共享库。
// ============================================================

// RNNoise 函数指针类型
typedef struct DenoiseState DenoiseState;
typedef DenoiseState* (*RnnoiseCreateFn)(const float* model);
typedef float (*RnnoiseProcessFrameFn)(DenoiseState* st, float* out, const float* in);
typedef void (*RnnoiseDestroyFn)(DenoiseState* st);
typedef int (*RnnoiseGetFrameSizeFn)();

static void* g_rnnoiseLib = nullptr;
static RnnoiseCreateFn g_rnnoiseCreate = nullptr;
static RnnoiseProcessFrameFn g_rnnoiseProcessFrame = nullptr;
static RnnoiseDestroyFn g_rnnoiseDestroy = nullptr;

// ============================================================
// NoiseSuppressor
// ============================================================

NoiseSuppressor::NoiseSuppressor() {
    m_tailBuffer.reserve(kFrameSize);
}

NoiseSuppressor::~NoiseSuppressor() {
    Reset();
}

bool NoiseSuppressor::Init() {
    if (m_initialized) return true;

    // 动态加载 RNNoise 库
    if (!g_rnnoiseLib) {
        g_rnnoiseLib = dlopen("librnnoise.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_rnnoiseLib) {
            // 回退到无版本后缀
            g_rnnoiseLib = dlopen("librnnoise.so.0", RTLD_NOW | RTLD_LOCAL);
        }
        if (g_rnnoiseLib) {
            g_rnnoiseCreate = (RnnoiseCreateFn)dlsym(g_rnnoiseLib, "rnnoise_create");
            g_rnnoiseProcessFrame = (RnnoiseProcessFrameFn)dlsym(g_rnnoiseLib, "rnnoise_process_frame");
            g_rnnoiseDestroy = (RnnoiseDestroyFn)dlsym(g_rnnoiseLib, "rnnoise_destroy");

            if (!g_rnnoiseCreate || !g_rnnoiseProcessFrame || !g_rnnoiseDestroy) {
                OH_LOG_WARN(LOG_APP, "%{public}s: RNNoise symbols not found, disabling", LOG_TAG);
                dlclose(g_rnnoiseLib);
                g_rnnoiseLib = nullptr;
                return false;
            }
            OH_LOG_INFO(LOG_APP, "%{public}s: RNNoise loaded OK", LOG_TAG);
        } else {
            OH_LOG_INFO(LOG_APP, "%{public}s: RNNoise not available, noise suppression disabled", LOG_TAG);
            return false;
        }
    }

    // 创建 RNNoise 实例（使用默认内置模型）
    m_rnnState = g_rnnoiseCreate(nullptr);
    if (!m_rnnState) {
        OH_LOG_ERROR(LOG_APP, "%{public}s: rnnoise_create failed", LOG_TAG);
        return false;
    }

    m_initialized = true;
    m_tailBuffer.clear();
    OH_LOG_INFO(LOG_APP, "%{public}s: initialized", LOG_TAG);
    return true;
}

void NoiseSuppressor::Reset() {
    if (m_rnnState && g_rnnoiseDestroy) {
        g_rnnoiseDestroy(m_rnnState);
        m_rnnState = nullptr;
    }
    m_initialized = false;
    m_tailBuffer.clear();
    m_lastVadProb = 0.0f;
}

void NoiseSuppressor::ProcessFrame(const float* in, float* out) {
    if (!m_rnnState || !g_rnnoiseProcessFrame) return;
    m_lastVadProb = g_rnnoiseProcessFrame(m_rnnState, out, in);
}

void NoiseSuppressor::Process(float* samples, size_t n) {
    if (!m_initialized || n == 0) return;

    // 先处理上次残留的尾帧
    if (!m_tailBuffer.empty()) {
        size_t need = kFrameSize - m_tailBuffer.size();
        size_t take = (n < need) ? n : need;

        m_tailBuffer.insert(m_tailBuffer.end(), samples, samples + take);

        if (m_tailBuffer.size() >= kFrameSize) {
            float output[kFrameSize];
            ProcessFrame(m_tailBuffer.data(), output);
            // 将输出写回 samples 开头
            memcpy(samples, output, kFrameSize * sizeof(float));
            m_tailBuffer.clear();

            // 更新指针继续处理剩余数据
            samples += take;
            n -= take;
        } else {
            return; // 还不够一帧，等待更多数据
        }
    }

    // 处理完整的帧
    size_t frames = n / kFrameSize;
    for (size_t i = 0; i < frames; i++) {
        float output[kFrameSize];
        ProcessFrame(samples + i * kFrameSize, output);
        memcpy(samples + i * kFrameSize, output, kFrameSize * sizeof(float));
    }

    // 剩余的不足一帧的存入 tailBuffer
    size_t remaining = n % kFrameSize;
    if (remaining > 0) {
        m_tailBuffer.assign(samples + frames * kFrameSize, samples + frames * kFrameSize + remaining);
    }
}
