#ifndef REMOTE_LLM_ENGINE_H
#define REMOTE_LLM_ENGINE_H

#include "LlmEngine.h"
#include <mutex>
#include <atomic>

/**
 * @brief 远端 LLM 引擎 — 通过 HTTP 调用 Ollama/OpenAI API
 * 
 * 此引擎的调用实际在 ArkTS 侧通过 @ohos.net.http 实现，
 * C++ 侧仅保留接口定义和配置管理。
 * 
 * 当需要从 C++ 侧调用远端 LLM 时，通过 NAPI 回调到 ArkTS 执行 HTTP 请求。
 */

class RemoteLlmEngine : public LlmEngine {
public:
    RemoteLlmEngine();
    ~RemoteLlmEngine() override;

    bool Init(const LlmConfig& config) override;

    void CallStreaming(
        const std::vector<std::pair<std::string, std::string>>& messages,
        LlmStreamCallback onToken,
        LlmCompleteCallback onComplete
    ) override;

    std::string Call(const std::vector<std::pair<std::string, std::string>>& messages) override;

    void Release() override;
    bool IsInitialized() const override { return m_initialized.load(); }
    Type GetType() const override { return Type::Remote; }

private:
    std::atomic<bool> m_initialized{false};
    LlmConfig m_config;
    std::mutex m_mutex;
};

#endif // REMOTE_LLM_ENGINE_H
