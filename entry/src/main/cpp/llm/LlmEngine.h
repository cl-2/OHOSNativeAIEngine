#ifndef LLM_ENGINE_H
#define LLM_ENGINE_H

#include <string>
#include <functional>
#include <memory>
#include <vector>

/**
 * @brief LLM 引擎统一接口
 * 
 * 本地（ONNX Runtime）和云端（HTTP）LLM 统一通过此接口调用，
 * Index.ets 无需关心底层实现，通过 NAPI 切换即可。
 */

// 流式回调：每次生成一个 token（或一段文本）时调用
// 返回 true 继续生成，返回 false 停止生成
using LlmStreamCallback = std::function<bool(const std::string& token)>;

// 完成回调：全部生成完成时调用
using LlmCompleteCallback = std::function<void(const std::string& fullText, bool success)>;

struct LlmConfig {
    // 本地模型路径（本地模式用）
    std::string modelPath;       // model.onnx 路径
    std::string tokenizerPath;   // tokenizer.json 路径
    
    // 远程 API 配置（云端模式用）
    std::string apiUrl;
    std::string apiKey;
    std::string modelName;
    
    // 系统提示词
    std::string systemPrompt = "你是一个智能AI助手，请用简洁友好的方式回答用户的问题。";
    
    // 生成参数
    int maxTokens = 1024;
    float temperature = 0.7f;
};

class LlmEngine {
public:
    virtual ~LlmEngine() = default;

    /// 初始化引擎
    virtual bool Init(const LlmConfig& config) = 0;

    /// 流式调用 LLM
    /// @param messages 对话历史（含 system/user/assistant 角色）
    /// @param onToken 每生成一个 token 回调
    /// @param onComplete 生成完成回调
    virtual void CallStreaming(
        const std::vector<std::pair<std::string, std::string>>& messages,
        LlmStreamCallback onToken,
        LlmCompleteCallback onComplete
    ) = 0;

    /// 同步调用（一次性获取完整回复）
    virtual std::string Call(const std::vector<std::pair<std::string, std::string>>& messages) = 0;

    /// 释放资源
    virtual void Release() = 0;

    /// 是否已初始化
    virtual bool IsInitialized() const = 0;

    virtual void CancelGeneration() {}
    virtual void SaveInterruptedContext() {}
    // Applies to the next generation only. A value of 0 keeps production
    // sampling. Diagnostic implementations may force a repeatable non-EOS
    // decode load for on-device benchmarks.
    virtual void SetDiagnosticDecodeTokens(int tokens) { (void)tokens; }

    /// 引擎类型
    enum class Type { Local, Remote };
    virtual Type GetType() const = 0;
};

#endif // LLM_ENGINE_H
