#ifndef HTTP_LLM_CLIENT_H
#define HTTP_LLM_CLIENT_H

#include "LlmClient.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

#ifdef _WIN32
// Windows 的 SendMessage 宏会污染我们的 SendMessage 方法名
#pragma push_macro("SendMessage")
#undef SendMessage
#include <windows.h>
#include <winhttp.h>
#pragma pop_macro("SendMessage")
#endif

/**
 * @brief 基于 WinHTTP 的 LLM 客户端实现 (Windows)
 * 
 * 支持 OpenAI 兼容 API 的流式和非流式调用。
 * 支持 Ollama, DeepSeek, 通义千问等。
 */
class HttpLlmClient : public LlmClient {
public:
    HttpLlmClient();
    ~HttpLlmClient() override;

    void Init(const LlmConfig& config) override;
    bool SendMessage(const std::string& text) override;
    bool SendMessageStream(const std::string& text) override;
    void Stop() override;
    std::vector<LlmMessage> GetHistory() const override;
    void ClearHistory() override;
    void SetSystemPrompt(const std::string& prompt) override;

    // Windows API 有 SendMessageA/W 宏，需要 undef
    // 使用 DoSendMessage 替代防止宏冲突

private:
    LlmConfig m_config;
    std::vector<LlmMessage> m_history;
    std::string m_systemPrompt;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_stopping{false};
    std::thread m_requestThread;

#ifdef _WIN32
    // WinHTTP 句柄
    HINTERNET m_session = nullptr;
    HINTERNET m_connection = nullptr;

    bool EnsureSession();
    std::string BuildRequestBody(const std::string& userText, bool stream);
    bool ParseSseLine(const std::string& line, std::string& content);
    void RequestThreadProc(const std::string& text, bool stream);
#endif

    // 内部发送方法 (避免 Windows SendMessage 宏冲突)
    bool DoSendMessage(const std::string& text);
    bool DoSendMessageStream(const std::string& text);
};

#endif // HTTP_LLM_CLIENT_H
