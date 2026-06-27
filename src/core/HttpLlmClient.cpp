#include "HttpLlmClient.h"
#include "Logger.h"
#include <sstream>

// ============================================================
// HttpLlmClient — Windows WinHTTP 实现的 LLM 客户端
// ============================================================

HttpLlmClient::HttpLlmClient() {
}

HttpLlmClient::~HttpLlmClient() {
    Stop();
}

void HttpLlmClient::Init(const LlmConfig& config) {
    m_config = config;
    m_systemPrompt = "You are a helpful AI assistant. Answer the user's questions concisely and friendly.";
    LOGI("HttpLlmClient initialized: url=%s, model=%s",
         config.apiUrl.c_str(), config.modelName.c_str());
}

bool HttpLlmClient::SendMessage(const std::string& text) {
    return DoSendMessage(text);
}

bool HttpLlmClient::SendMessageStream(const std::string& text) {
    return DoSendMessageStream(text);
}

bool HttpLlmClient::DoSendMessage(const std::string& text) {
    m_stopping.store(false);
    LOGI("HttpLlmClient::DoSendMessage: %s", text.c_str());

    m_requestThread = std::thread(&HttpLlmClient::RequestThreadProc, this, text, false);
    m_requestThread.detach();
    return true;
}

bool HttpLlmClient::DoSendMessageStream(const std::string& text) {
    m_stopping.store(false);
    LOGI("HttpLlmClient::DoSendMessageStream: %s", text.c_str());

    m_requestThread = std::thread(&HttpLlmClient::RequestThreadProc, this, text, true);
    m_requestThread.detach();
    return true;
}

void HttpLlmClient::Stop() {
    m_stopping.store(true);
}

std::vector<LlmMessage> HttpLlmClient::GetHistory() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_history;
}

void HttpLlmClient::ClearHistory() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_history.clear();
}

void HttpLlmClient::SetSystemPrompt(const std::string& prompt) {
    m_systemPrompt = prompt;
}

// ============================================================
// WinHTTP 实现
// ============================================================

#ifdef _WIN32

bool HttpLlmClient::EnsureSession() {
    if (m_session) return true;

    m_session = WinHttpOpen(L"NativeAIEngine/1.0",
                            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            nullptr, nullptr, 0);
    if (!m_session) {
        LOGE("WinHttpOpen failed: %lu", GetLastError());
        return false;
    }

    // 设置超时
    WinHttpSetTimeouts(m_session, 5000, 15000, 30000, 30000);
    return true;
}

std::string HttpLlmClient::BuildRequestBody(const std::string& userText, bool stream) {
    // 构建 OpenAI 兼容的请求体
    std::stringstream ss;

    ss << "{\n";
    ss << "  \"model\": \"" << m_config.modelName << "\",\n";
    ss << "  \"messages\": [\n";
    ss << "    {\"role\": \"system\", \"content\": \"" << m_systemPrompt << "\"},\n";

    // 对话历史
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& msg : m_history) {
        ss << "    {\"role\": \"" << msg.role << "\", \"content\": \"" << msg.content << "\"},\n";
    }

    // 当前用户消息
    ss << "    {\"role\": \"user\", \"content\": \"" << userText << "\"}\n";
    ss << "  ],\n";
    ss << "  \"stream\": " << (stream ? "true" : "false") << ",\n";
    ss << "  \"max_tokens\": " << m_config.maxTokens << ",\n";
    ss << "  \"temperature\": " << m_config.temperature << "\n";
    ss << "}\n";

    return ss.str();
}

void HttpLlmClient::RequestThreadProc(const std::string& text, bool stream) {
    if (!EnsureSession()) {
        if (OnError) OnError("WinHTTP session failed");
        return;
    }

    // 解析 URL
    std::string url = m_config.apiUrl;
    std::string host, path;
    bool isHttps = false;

    if (url.find("https://") == 0) {
        isHttps = true;
        url = url.substr(8);
    } else if (url.find("http://") == 0) {
        url = url.substr(7);
    }

    auto slashPos = url.find('/');
    if (slashPos != std::string::npos) {
        host = url.substr(0, slashPos);
        path = url.substr(slashPos);
    } else {
        host = url;
        path = "/v1/chat/completions";
    }

    // 解析端口号 (host:port)
    INTERNET_PORT port = isHttps ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    auto colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        port = static_cast<INTERNET_PORT>(std::stoi(host.substr(colonPos + 1)));
        host = host.substr(0, colonPos);
    }

    // 转换到宽字符
    int hostLen = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
    int pathLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring whost(hostLen, L'\0');
    std::wstring wpath(pathLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &whost[0], hostLen);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], pathLen);

    // 打开连接 (使用解析后的端口)
    LOGI("WinHttpConnect: host=%s port=%d", host.c_str(), port);
    HINTERNET connect = WinHttpConnect(m_session, whost.c_str(), port, 0);
    if (!connect) {
        LOGE("WinHttpConnect failed: %lu", GetLastError());
        if (OnError) OnError("Connection failed");
        return;
    }

    // 创建请求
    LOGI("WinHttpOpenRequest: path=%s", path.c_str());
    HINTERNET request = WinHttpOpenRequest(connect, L"POST", wpath.c_str(),
                                           nullptr, nullptr, nullptr,
                                           isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        LOGE("WinHttpOpenRequest failed: %lu", GetLastError());
        WinHttpCloseHandle(connect);
        if (OnError) OnError("Request creation failed");
        return;
    }

    // 设置请求头
    std::string headers = "Content-Type: application/json\r\n";
    if (!m_config.apiKey.empty()) {
        headers += "Authorization: Bearer " + m_config.apiKey + "\r\n";
    }
    if (stream) {
        headers += "Accept: text/event-stream\r\n";
    }

    int headersLenW = MultiByteToWideChar(CP_UTF8, 0, headers.c_str(), -1, nullptr, 0);
    std::wstring wheaders(headersLenW, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, headers.c_str(), -1, &wheaders[0], headersLenW);

    WinHttpAddRequestHeaders(request, wheaders.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    // 发送请求
    std::string body = BuildRequestBody(text, stream);
    if (!WinHttpSendRequest(request, nullptr, 0,
                            (void*)body.c_str(), (DWORD)body.size(),
                            (DWORD)body.size(), 0)) {
        LOGE("WinHttpSendRequest failed: %lu", GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        if (OnError) OnError("Send request failed");
        return;
    }

    LOGI("WinHttpReceiveResponse...");
    if (!WinHttpReceiveResponse(request, nullptr)) {
        LOGE("WinHttpReceiveResponse failed: %lu", GetLastError());
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        if (OnError) OnError("Receive response failed");
        return;
    }
    LOGI("WinHttpReceiveResponse OK");

    // 读取响应
    std::string response;
    std::string fullResponse;
    DWORD bytesRead = 0;
    char buffer[4096];

    int readLoopCount = 0;
    while (!m_stopping.load()) {
        bytesRead = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer) - 1, &bytesRead)) {
            DWORD err = GetLastError();
            LOGI("WinHttpReadData err=%lu", err);
            if (err != ERROR_MORE_DATA) break;
        }
        if (bytesRead == 0) {
            LOGI("WinHttpReadData EOF, total_read=%zu", response.size());
            break;
        }
        if (readLoopCount++ % 10 == 0)
            LOGI("WinHttpReadData chunk=%lu total=%zu", bytesRead, response.size());

        buffer[bytesRead] = '\0';
        response.append(buffer, bytesRead);

        // 流式处理 SSE
        LOGI("Raw response (%zu bytes): %.200s", response.size(), response.c_str());
        if (stream) {
            // 按行解析
            size_t pos = 0;
            while (true) {
                size_t nl = response.find('\n', pos);
                if (nl == std::string::npos) {
                    // 保留不完整的行
                    if (pos > 0) response = response.substr(pos);
                    break;
                }
                std::string line = response.substr(pos, nl - pos);
                pos = nl + 1;

                if (line.find("data: ") == 0) {
                    std::string data = line.substr(6);
                    if (data == "[DONE]") {
                        // 流结束
                    } else {
                        // 从 OpenAI SSE JSON 中提取 content 字段
                        // 格式: {"choices":[{"delta":{"content":"text"}}]}
                        std::string content;
                        auto cpos = data.find("\"content\":\"");
                        if (cpos != std::string::npos) {
                            cpos += 11; // skip past \"content\":\"
                            auto end = data.find('"', cpos);
                            if (end != std::string::npos) {
                                content = data.substr(cpos, end - cpos);
                                // 处理转义字符
                                size_t esc = 0;
                                while ((esc = content.find("\\n", esc)) != std::string::npos) {
                                    content.replace(esc, 2, "\n");
                                    esc++;
                                }
                                esc = 0;
                                while ((esc = content.find("\\\"", esc)) != std::string::npos) {
                                    content.replace(esc, 2, "\"");
                                    esc++;
                                }
                            }
                        }
                        // 只传递提取的文本内容，忽略元数据块(如 role, finish_reason)
                        if (!content.empty()) {
                            if (OnStreamChunk) OnStreamChunk(content);
                            fullResponse += content;
                        }
                    }
                }
            }
        }
    }

    if (!stream) {
        // 非流式: 直接回调完整响应
        LOGI("LLM non-stream response, len=%zu", response.size());
        if (OnResponse) OnResponse(response);
    } else {
        LOGI("LLM stream complete, full_len=%zu", fullResponse.size());
        // 流结束时也触发 OnResponse 让 UI 知道完成
        if (OnResponse) OnResponse(fullResponse);
        if (OnStreamComplete) OnStreamComplete(fullResponse);
    }

    // 更新历史
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_history.push_back({"user", text});
        m_history.push_back({"assistant", stream ? fullResponse : response});
        if (m_history.size() > 20) {
            m_history.erase(m_history.begin(), m_history.begin() + (m_history.size() - 20));
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    LOGI("HttpLlmClient request complete");
}

#endif // _WIN32
