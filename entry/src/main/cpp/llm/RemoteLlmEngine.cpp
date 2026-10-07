#include "RemoteLlmEngine.h"
#include <hilog/log.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "OHOS_RemoteLlm"

RemoteLlmEngine::RemoteLlmEngine() {
    OH_LOG_INFO(LOG_APP, "OHOS_RemoteLlm: created");
}

RemoteLlmEngine::~RemoteLlmEngine() {
    RemoteLlmEngine::Release();
}

bool RemoteLlmEngine::Init(const LlmConfig& config) {
    m_config = config;
    m_initialized.store(true);
    OH_LOG_INFO(LOG_APP, "OHOS_RemoteLlm: init, url=%{public}s", config.apiUrl.c_str());
    return true;
}

void RemoteLlmEngine::CallStreaming(
    const std::vector<std::pair<std::string, std::string>>& messages,
    LlmStreamCallback onToken,
    LlmCompleteCallback onComplete)
{
    // HTTP 调用在 ArkTS 侧实现
    // 此方法仅在从 C++ 侧需要调用远端 LLM 时使用
    // 目前 ArkTS 直接调用 HTTP，此方法保留为扩展
    OH_LOG_INFO(LOG_APP, "OHOS_RemoteLlm: CallStreaming - delegate to ArkTS");
    if (onComplete) onComplete("", false);
}

std::string RemoteLlmEngine::Call(
    const std::vector<std::pair<std::string, std::string>>& messages)
{
    return "";
}

void RemoteLlmEngine::Release() {
    m_initialized.store(false);
}
