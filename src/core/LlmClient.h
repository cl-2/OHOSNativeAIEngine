#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <string>
#include <functional>
#include <vector>

/**
 * @brief LLM 请求消息
 */
struct LlmMessage {
    std::string role;    // "system", "user", "assistant"
    std::string content;
};

/**
 * @brief LLM 模型配置
 */
struct LlmConfig {
    std::string apiUrl;      // API 端点 URL
    std::string apiKey;      // API Key (可为空)
    std::string modelName;   // 模型名称
    int maxTokens = 1024;    // 最大生成 token 数
    double temperature = 0.7;
    int connectTimeoutMs = 15000;
    int readTimeoutMs = 30000;
};

/**
 * @brief LLM 客户端纯虚接口
 * 
 * 支持 OpenAI 兼容 API (Ollama, DeepSeek, 通义千问等)。
 * Windows: WinHTTP 实现
 * HarmonyOS: @ohos.net.http 实现 (ArkTS 层)
 */
class LlmClient {
public:
    virtual ~LlmClient() = default;

    /// 初始化客户端
    virtual void Init(const LlmConfig& config) = 0;

    /// 发送消息 (非流式)
    virtual bool SendMessage(const std::string& text) = 0;

    /// 发送消息 (流式, SSE)
    virtual bool SendMessageStream(const std::string& text) = 0;

    /// 停止当前请求
    virtual void Stop() = 0;

    /// 获取对话历史
    virtual std::vector<LlmMessage> GetHistory() const = 0;

    /// 清空对话历史
    virtual void ClearHistory() = 0;

    /// 设置系统提示词
    virtual void SetSystemPrompt(const std::string& prompt) = 0;

    // ========== 回调 ==========

    /// 完整响应回调 (非流式)
    std::function<void(const std::string& response)> OnResponse;

    /// 流式响应回调 (每块)
    std::function<void(const std::string& chunk)> OnStreamChunk;

    /// 流式完成回调
    std::function<void(const std::string& fullResponse)> OnStreamComplete;

    /// 错误回调
    std::function<void(const std::string& error)> OnError;
};

#endif // LLM_CLIENT_H
