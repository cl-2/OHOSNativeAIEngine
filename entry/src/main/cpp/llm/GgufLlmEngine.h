#ifndef GGUF_LLM_ENGINE_H
#define GGUF_LLM_ENGINE_H

#include "LlmEngine.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

struct llama_context;
struct llama_model;
struct llama_vocab;

class GgufLlmEngine : public LlmEngine {
public:
    GgufLlmEngine();
    ~GgufLlmEngine() override;

    bool Init(const LlmConfig& config) override;
    void CallStreaming(
        const std::vector<std::pair<std::string, std::string>>& messages,
        LlmStreamCallback onToken,
        LlmCompleteCallback onComplete) override;
    std::string Call(const std::vector<std::pair<std::string, std::string>>& messages) override;
    void Release() override;
    bool IsInitialized() const override { return m_initialized.load(); }
    Type GetType() const override { return Type::Local; }
    void CancelGeneration() override;
    void SaveInterruptedContext() override;
    void SetDiagnosticDecodeTokens(int tokens) override;

private:
    std::string BuildPrompt(const std::vector<std::pair<std::string, std::string>>& messages);
    bool Tokenize(const std::string& text, std::vector<int>& tokens) const;
    std::string TokenToPiece(int token) const;
    static bool AbortCallback(void* data);
    void GenerationFinished();

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_abortRequested{false};
    std::atomic<int> m_diagnosticDecodeTokens{0};
    std::atomic<int> m_activeGenerations{0};
    std::mutex m_engineMutex;
    std::mutex m_activeMutex;
    std::condition_variable m_activeCv;
    std::mutex m_contextMutex;

    llama_model* m_model = nullptr;
    llama_context* m_context = nullptr;
    const llama_vocab* m_vocab = nullptr;
    bool m_backendInitialized = false;

    LlmConfig m_config;
    std::string m_partialResponse;
    std::string m_interruptedPartialText;
    int m_contextTokens = 1024;
    int m_batchTokens = 128;
    int m_threads = 4;
    int m_maxNewTokens = 512;
};

#endif
