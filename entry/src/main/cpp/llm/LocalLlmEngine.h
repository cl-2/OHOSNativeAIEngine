#ifndef LOCAL_LLM_ENGINE_H
#define LOCAL_LLM_ENGINE_H

#include "LlmEngine.h"
#include "BpeTokenizer.h"
#include "utils/CancelScope.h"
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <condition_variable>

struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtValue;
struct OrtMemoryInfo;
struct OrtAllocator;
struct OrtRunOptions;
struct OrtApi;
struct OrtStatus;

/**
 * @brief 本地 LLM 引擎 — 基于 ONNX Runtime 的 Qwen2.5-0.5B 推理
 *
 * 增量上下文（Item 3）：
 *   当用户打断 AI 回答时，SaveInterruptedContext() 会保存当时
 *   已生成的部分文本。下一次 CallStreaming 构建 prompt 时自动
 *   注入为 "[AI 正在说: ...]" 上下文，让 LLM 知道被打断前的情况。
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

    /// 中断当前正在进行的推理
    void CancelGeneration() override { m_cancelScope.Cancel(); }

    // ========== 增量上下文（Item 3）==========

    /// 中断时保存当前已生成的部分文本
    void SaveInterruptedContext() override;

    /// 获取并清除保存的打断上下文
    std::string ConsumeInterruptedContext();

    /// 是否有未消费的打断上下文
    bool HasInterruptedContext() const { return !m_interruptedPartialText.empty(); }

private:
    bool LoadOrtRuntime(const std::string& libPath);
    bool CreateSession(const std::string& modelPath);

    bool RunStepWithCache(const std::vector<int32_t>& inputIds,
                          std::vector<float>& logits,
                          std::vector<OrtValue*>& kvCache,
                          int64_t& pastLength);
    void ReleaseKvCache(std::vector<OrtValue*>& kvCache);
    void LogOrtError(const char* operation, OrtStatus* status);

    int32_t SampleToken(const float* logits, int vocabSize);
    void ApplyFilters(std::vector<std::pair<int32_t, float>>& candidates);

    bool FileExists(const std::string& path);
    void Log(const char* msg);

    /// 构建 prompt，如有增量上下文则注入
    std::string BuildPrompt(const std::vector<std::pair<std::string, std::string>>& messages);

    static std::string BuildQwenPrompt(
        const std::vector<std::pair<std::string, std::string>>& messages);

    // ========== 状态 ==========

    std::atomic<bool> m_initialized{false};
    CancelScope m_cancelScope;
    std::mutex m_mutex;
    std::atomic<int> m_activeGenerations{0};
    std::mutex m_activeMutex;
    std::condition_variable m_activeCv;

    std::unique_ptr<BpeTokenizer> m_tokenizer;

    // ONNX Runtime
    const OrtApi* m_ortApi = nullptr;
    void* m_ortHandle = nullptr;
    OrtEnv* m_ortEnv = nullptr;
    OrtSession* m_session = nullptr;
    OrtMemoryInfo* m_memoryInfo = nullptr;
    OrtAllocator* m_allocator = nullptr;
    std::vector<std::string> m_ortInputNames;
    std::vector<std::string> m_ortOutputNames;

    std::string m_modelPath;
    std::string m_tokenizerPath;
    int32_t m_vocabSize = 0;

    LlmConfig m_config;
    float m_temperature = 0.7f;
    int m_topK = 40;
    float m_topP = 0.9f;
    int m_maxNewTokens = 512;
    // 当前 ONNX 模型没有 past_key_values 输入输出，RunStep 会生成整段 logits。
    // 限制单步上下文，避免长对话产生数百 MB 到 GB 级临时张量。
    int m_maxContextTokens = 512;

    // ========== 增量上下文 ==========
    std::string m_partialResponse;          // 当前生成中的部分文本（推理线程写入）
    std::string m_interruptedPartialText;   // 打断时保存的部分文本（跨线程安全）
    std::mutex m_contextMutex;
};

#endif // LOCAL_LLM_ENGINE_H
