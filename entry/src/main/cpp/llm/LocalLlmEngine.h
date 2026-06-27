#ifndef LOCAL_LLM_ENGINE_H
#define LOCAL_LLM_ENGINE_H

#include "LlmEngine.h"
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>

/**
 * @brief 本地 LLM 引擎 — 基于 ONNX Runtime
 * 
 * 运行 Qwen2.5-0.5B-Instruct ONNX 模型，完全离线，数据不出端。
 * 
 * 模型文件需要放置在 assets/models/llm/ 目录：
 *   - model.int4.onnx      (INT4 量化模型，~350MB)
 *   - tokenizer.json        (HuggingFace 格式分词器)
 *   - config.json           (模型配置)
 * 
 * 下载链接见 README 或本文档末尾注释。
 */

class LocalLlmEngine : public LlmEngine {
public:
    LocalLlmEngine();
    ~LocalLlmEngine() override;

    bool Init(const LlmConfig& config) override;
    
    void CallStreaming(
        const std::vector<std::pair<std::string, std::string>>& messages,
        LlmStreamCallback onToken,
        LlmCompleteCallback onComplete
    ) override;

    std::string Call(const std::vector<std::pair<std::string, std::string>>& messages) override;

    void Release() override;
    bool IsInitialized() const override { return m_initialized.load(); }
    Type GetType() const override { return Type::Local; }

private:
    // ========== ONNX Runtime 相关 ==========
    
    /// 加载 ONNX Runtime 和模型
    bool LoadModel(const std::string& modelPath);
    
    /// 运行推理
    bool RunInference(const std::vector<int32_t>& inputIds, 
                      std::vector<int32_t>& outputIds,
                      int maxNewTokens);

    // ========== 分词器 ==========
    
    /// 加载 tokenizer.json
    bool LoadTokenizer(const std::string& tokenizerPath);
    
    /// 编码：文本 → token IDs
    std::vector<int32_t> Encode(const std::string& text);
    
    /// 解码：token IDs → 文本
    std::string Decode(const std::vector<int32_t>& ids);

    // ========== 对话模板 ==========
    
    /// 构建 Qwen2.5 格式的对话 prompt
    std::string BuildPrompt(const std::vector<std::pair<std::string, std::string>>& messages);

    // ========== 状态 ==========

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_generating{false};
    std::mutex m_mutex;

    // ONNX Runtime 句柄（通过 dlopen 加载）
    void* m_ortHandle = nullptr;
    
    // 模型路径
    std::string m_modelPath;
    std::string m_tokenizerPath;

    // 分词器数据（完整 tokenizer.json 内容）
    std::string m_tokenizerJson;

    // 配置
    LlmConfig m_config;

    // ========== 日志 ==========
    void Log(const char* msg);
};

#endif // LOCAL_LLM_ENGINE_H
