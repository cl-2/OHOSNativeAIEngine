#include "LocalLlmEngine.h"
#include "onnxruntime/onnxruntime_c_api.h"
#include "metrics/MetricsCollector.h"
#include <hilog/log.h>

#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "OHOS_LocalLlm"

// ============================================================
// 简单 xorshift64 PRNG（用于采样，避免 std::random_device 依赖）
// ============================================================

static thread_local struct {
    uint64_t state = 123456789;
    uint64_t next() { uint64_t x = state; x ^= x << 13; x ^= x >> 7; x ^= x << 17; state = x; return x; }
    float nextFloat() { return (next() & 0x7FFFFFFF) / (float)0x7FFFFFFF; }
} g_rng;

// ============================================================
// LocalLlmEngine
// ============================================================

LocalLlmEngine::LocalLlmEngine() {
    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: created");
}

LocalLlmEngine::~LocalLlmEngine() {
    LocalLlmEngine::Release();
}

void LocalLlmEngine::Log(const char* msg) {
    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: %{public}s", msg);
}

bool LocalLlmEngine::FileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// ============================================================
// Init / Release
// ============================================================

bool LocalLlmEngine::Init(const LlmConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: Init, model=%{public}s", config.modelPath.c_str());

    m_modelPath = config.modelPath;
    m_tokenizerPath = config.tokenizerPath;
    m_config = config;
    m_temperature = config.temperature;
    m_maxNewTokens = config.maxTokens;

    // 1. 加载分词器
    m_tokenizer = std::make_unique<BpeTokenizer>();
    if (!m_tokenizer->Load(m_tokenizerPath)) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: failed to load tokenizer from %{public}s",
            m_tokenizerPath.c_str());
        return false;
    }
    Log("Tokenizer loaded");
    m_vocabSize = m_tokenizer->VocabSize();

    // 2. 加载 ONNX Runtime
    if (!LoadOrtRuntime("libonnxruntime.so.1.18.0")) {
        // 回退：尝试无版本号的库名
        if (!LoadOrtRuntime("libonnxruntime.so")) {
            OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: failed to load ONNX Runtime");
            return false;
        }
    }
    Log("ONNX Runtime loaded");

    // 3. 加载模型
    if (!CreateSession(m_modelPath)) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: failed to create session from %{public}s",
            m_modelPath.c_str());
        return false;
    }
    Log("Model session created");

    m_initialized.store(true);
    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: Init done, vocab=%{public}d", m_vocabSize);
    return true;
}

void LocalLlmEngine::Release() {
    // 先发取消，再等待detach推理线程退出，防止释放Session后线程继续访问this。
    m_cancelScope.Cancel();
    {
        std::unique_lock<std::mutex> activeLock(m_activeMutex);
        m_activeCv.wait(activeLock, [this]() { return m_activeGenerations.load() == 0; });
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_initialized.store(false);

    if (m_ortApi && m_session) {
        m_ortApi->ReleaseSession(m_session);
        m_session = nullptr;
    }
    if (m_ortApi && m_ortEnv) {
        m_ortApi->ReleaseEnv(m_ortEnv);
        m_ortEnv = nullptr;
    }
    if (m_memoryInfo && m_ortApi) {
        m_ortApi->ReleaseMemoryInfo(m_memoryInfo);
        m_memoryInfo = nullptr;
    }
    m_ortApi = nullptr;
    if (m_ortHandle) {
        dlclose(m_ortHandle);
        m_ortHandle = nullptr;
    }
    m_tokenizer.reset();
    Log("Released");
}

// ============================================================
// ONNX Runtime 动态加载
// ============================================================

bool LocalLlmEngine::LoadOrtRuntime(const std::string& libPath) {
    // 先尝试 RTLD_NOLOAD（已经由 sherpa-onnx 加载了）
    m_ortHandle = dlopen(libPath.c_str(), RTLD_NOLOAD);
    if (!m_ortHandle) {
        m_ortHandle = dlopen(libPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    }
    if (!m_ortHandle) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: dlopen(%{public}s) failed: %{public}s",
            libPath.c_str(), dlerror());
        return false;
    }

    // 获取 OrtGetApiBase
    auto getApiBase = (const OrtApiBase* (*)())dlsym(m_ortHandle, "OrtGetApiBase");
    if (!getApiBase) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: dlsym OrtGetApiBase failed: %{public}s", dlerror());
        dlclose(m_ortHandle);
        m_ortHandle = nullptr;
        return false;
    }

    const OrtApiBase* apiBase = getApiBase();
    if (!apiBase) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: OrtGetApiBase returned null");
        dlclose(m_ortHandle);
        m_ortHandle = nullptr;
        return false;
    }

    // The function-table layout is versioned; an older fallback is ABI-unsafe.
    m_ortApi = apiBase->GetApi(ORT_API_VERSION);
    if (!m_ortApi) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: GetApi(%{public}d) failed", ORT_API_VERSION);
        dlclose(m_ortHandle);
        m_ortHandle = nullptr;
        return false;
    }

    // 创建环境
    OrtStatus* status = m_ortApi->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "local_llm", &m_ortEnv);
    if (status) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: CreateEnv failed (status != null)");
        dlclose(m_ortHandle);
        m_ortHandle = nullptr;
        return false;
    }

    status = m_ortApi->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &m_memoryInfo);
    if (status) {
        LogOrtError("CreateCpuMemoryInfo", status);
        Release();
        return false;
    }
    status = m_ortApi->GetAllocatorWithDefaultOptions(&m_allocator);
    if (status) {
        LogOrtError("GetAllocatorWithDefaultOptions", status);
        Release();
        return false;
    }

    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: ONNX Runtime loaded from %{public}s", libPath.c_str());
    return true;
}

// ============================================================
// 创建 Session
// ============================================================

bool LocalLlmEngine::CreateSession(const std::string& modelPath) {
    if (!m_ortApi || !m_ortEnv) return false;

    // 创建 session options
    OrtSessionOptions* opts = nullptr;
    OrtStatus* status = m_ortApi->CreateSessionOptions(&opts);
    if (status || !opts) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: CreateSessionOptions failed");
        return false;
    }

    // 优化级别
    m_ortApi->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
    m_ortApi->SetIntraOpNumThreads(opts, 2);
    m_ortApi->SetInterOpNumThreads(opts, 1);

    // 创建 session
    status = m_ortApi->CreateSession(m_ortEnv, modelPath.c_str(), opts, &m_session);
    m_ortApi->ReleaseSessionOptions(opts);

    if (status || !m_session) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: CreateSession failed for %{public}s", modelPath.c_str());
        return false;
    }

    // 保存模型真实输入输出名。当前模型应为 51 输入、49 输出（24层KV Cache）。
    size_t numInputs = 0, numOutputs = 0;
    m_ortApi->SessionGetInputCount(m_session, &numInputs);
    m_ortApi->SessionGetOutputCount(m_session, &numOutputs);
    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: model has %{public}zu inputs, %{public}zu outputs",
        numInputs, numOutputs);

    m_ortInputNames.clear();
    m_ortOutputNames.clear();
    m_ortInputNames.reserve(numInputs);
    m_ortOutputNames.reserve(numOutputs);
    for (size_t i = 0; i < numInputs; i++) {
        char* name = nullptr;
        m_ortApi->SessionGetInputName(m_session, i, m_allocator, &name);
        if (name) {
            m_ortInputNames.emplace_back(name);
            OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: input[%{public}zu] = %{public}s", i, name);
            m_ortApi->AllocatorFree(m_allocator, name);
        }
    }
    for (size_t i = 0; i < numOutputs; i++) {
        char* name = nullptr;
        m_ortApi->SessionGetOutputName(m_session, i, m_allocator, &name);
        if (name) {
            m_ortOutputNames.emplace_back(name);
            OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: output[%{public}zu] = %{public}s", i, name);
            m_ortApi->AllocatorFree(m_allocator, name);
        }
    }

    if (m_ortInputNames.size() != 51 || m_ortOutputNames.size() != 49) {
        OH_LOG_ERROR(LOG_APP,
            "OHOS_LocalLlm: incompatible cache model inputs=%{public}zu outputs=%{public}zu",
            m_ortInputNames.size(), m_ortOutputNames.size());
        return false;
    }

    return true;
}

// ============================================================
// 单步推理
// ============================================================

void LocalLlmEngine::LogOrtError(const char* operation, OrtStatus* status) {
    if (!status || !m_ortApi) return;
    const char* message = m_ortApi->GetErrorMessage(status);
    OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: %{public}s failed: %{public}s",
        operation, message ? message : "unknown ORT error");
    m_ortApi->ReleaseStatus(status);
}

void LocalLlmEngine::ReleaseKvCache(std::vector<OrtValue*>& kvCache) {
    if (!m_ortApi) return;
    for (OrtValue* value : kvCache) {
        if (value) m_ortApi->ReleaseValue(value);
    }
    kvCache.clear();
}

bool LocalLlmEngine::RunStepWithCache(const std::vector<int32_t>& inputIds,
                                      std::vector<float>& logits,
                                      std::vector<OrtValue*>& kvCache,
                                      int64_t& pastLength) {
    constexpr size_t kLayerCount = 24;
    constexpr size_t kCacheTensorCount = kLayerCount * 2;
    constexpr int64_t kKvHeads = 2;
    constexpr int64_t kHeadDim = 64;
    if (!m_ortApi || !m_session || inputIds.empty() ||
        m_ortInputNames.size() != 3 + kCacheTensorCount ||
        m_ortOutputNames.size() != 1 + kCacheTensorCount) return false;

    const int64_t seqLen = static_cast<int64_t>(inputIds.size());
    const int64_t totalLength = pastLength + seqLen;
    const int64_t tokenShape[] = {1, seqLen};
    const int64_t maskShape[] = {1, totalLength};

    std::vector<int64_t> ids64(inputIds.begin(), inputIds.end());
    std::vector<int64_t> attentionMask(static_cast<size_t>(totalLength), 1);
    std::vector<int64_t> positionIds(static_cast<size_t>(seqLen));
    for (int64_t i = 0; i < seqLen; ++i) positionIds[static_cast<size_t>(i)] = pastLength + i;

    OrtValue* idsTensor = nullptr;
    OrtValue* maskTensor = nullptr;
    OrtValue* positionTensor = nullptr;
    OrtStatus* status = m_ortApi->CreateTensorWithDataAsOrtValue(
        m_memoryInfo, ids64.data(), ids64.size() * sizeof(int64_t),
        tokenShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &idsTensor);
    if (status) { LogOrtError("Create input_ids", status); return false; }
    status = m_ortApi->CreateTensorWithDataAsOrtValue(
        m_memoryInfo, attentionMask.data(), attentionMask.size() * sizeof(int64_t),
        maskShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &maskTensor);
    if (status) {
        LogOrtError("Create attention_mask", status);
        m_ortApi->ReleaseValue(idsTensor);
        return false;
    }
    status = m_ortApi->CreateTensorWithDataAsOrtValue(
        m_memoryInfo, positionIds.data(), positionIds.size() * sizeof(int64_t),
        tokenShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &positionTensor);
    if (status) {
        LogOrtError("Create position_ids", status);
        m_ortApi->ReleaseValue(idsTensor);
        m_ortApi->ReleaseValue(maskTensor);
        return false;
    }

    std::vector<OrtValue*> emptyCache;
    if (kvCache.empty()) {
        emptyCache.resize(kCacheTensorCount, nullptr);
        const int64_t emptyShape[] = {1, kKvHeads, 0, kHeadDim};
        for (size_t i = 0; i < kCacheTensorCount; ++i) {
            status = m_ortApi->CreateTensorAsOrtValue(
                m_allocator, emptyShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &emptyCache[i]);
            if (status) {
                LogOrtError("Create empty KV cache", status);
                ReleaseKvCache(emptyCache);
                m_ortApi->ReleaseValue(idsTensor);
                m_ortApi->ReleaseValue(maskTensor);
                m_ortApi->ReleaseValue(positionTensor);
                return false;
            }
        }
    }

    const std::vector<OrtValue*>& cacheInputs = kvCache.empty() ? emptyCache : kvCache;
    std::vector<const char*> inputNames;
    std::vector<const OrtValue*> inputValues;
    inputNames.reserve(m_ortInputNames.size());
    inputValues.reserve(m_ortInputNames.size());
    for (const std::string& name : m_ortInputNames) inputNames.push_back(name.c_str());
    inputValues.push_back(idsTensor);
    inputValues.push_back(maskTensor);
    inputValues.push_back(positionTensor);
    for (OrtValue* value : cacheInputs) inputValues.push_back(value);

    std::vector<const char*> outputNames;
    outputNames.reserve(m_ortOutputNames.size());
    for (const std::string& name : m_ortOutputNames) outputNames.push_back(name.c_str());
    std::vector<OrtValue*> outputs(m_ortOutputNames.size(), nullptr);

    status = m_ortApi->Run(m_session, nullptr,
        inputNames.data(), inputValues.data(), inputValues.size(),
        outputNames.data(), outputNames.size(), outputs.data());

    m_ortApi->ReleaseValue(idsTensor);
    m_ortApi->ReleaseValue(maskTensor);
    m_ortApi->ReleaseValue(positionTensor);
    ReleaseKvCache(emptyCache);

    if (status) {
        LogOrtError("Run with KV cache", status);
        for (OrtValue* output : outputs) if (output) m_ortApi->ReleaseValue(output);
        return false;
    }

    float* outputData = nullptr;
    status = m_ortApi->GetTensorMutableData(outputs[0], reinterpret_cast<void**>(&outputData));
    if (status || !outputData) {
        if (status) LogOrtError("Get logits", status);
        for (OrtValue* output : outputs) if (output) m_ortApi->ReleaseValue(output);
        return false;
    }

    OrtTensorTypeAndShapeInfo* shapeInfo = nullptr;
    size_t totalElements = 0;
    status = m_ortApi->GetTensorTypeAndShape(outputs[0], &shapeInfo);
    if (!status && shapeInfo) {
        status = m_ortApi->GetTensorShapeElementCount(shapeInfo, &totalElements);
        m_ortApi->ReleaseTensorTypeAndShapeInfo(shapeInfo);
    }
    if (status) {
        LogOrtError("Read logits shape", status);
        for (OrtValue* output : outputs) if (output) m_ortApi->ReleaseValue(output);
        return false;
    }

    const int32_t vocabSize = static_cast<int32_t>(totalElements / static_cast<size_t>(seqLen));
    const float* lastLogits = outputData + (seqLen - 1) * vocabSize;
    logits.assign(lastLogits, lastLogits + vocabSize);
    m_ortApi->ReleaseValue(outputs[0]);

    ReleaseKvCache(kvCache);
    kvCache.assign(outputs.begin() + 1, outputs.end());
    pastLength = totalLength;
    return true;
}

// ============================================================
// 采样
// ============================================================

void LocalLlmEngine::ApplyFilters(std::vector<std::pair<int32_t, float>>& candidates) {
    if (candidates.empty()) return;

    // 按 logit 降序排列
    std::sort(candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    float maxLogit = candidates[0].second;

    // Temperature 缩放 + softmax 计算
    double sumExp = 0.0;
    for (auto& [id, logit] : candidates) {
        logit = (logit - maxLogit) / m_temperature; // 数值稳定化
        logit = expf(logit);
        sumExp += logit;
    }

    // Top-P (nucleus) filtering
    std::vector<std::pair<int32_t, float>> filtered;
    double cumProb = 0.0;
    for (auto& [id, prob] : candidates) {
        float p = (float)(prob / sumExp);
        if (cumProb >= m_topP) break;
        filtered.push_back({id, p});
        cumProb += p;
    }

    if (filtered.empty()) {
        // Fallback: top-1 greedy
        filtered.push_back(candidates[0]);
        filtered[0].second = 1.0f;
    }

    candidates.swap(filtered);
}

int32_t LocalLlmEngine::SampleToken(const float* logits, int vocabSize) {
    // 收集所有候选 (token_id, logit)
    std::vector<std::pair<int32_t, float>> candidates;
    candidates.reserve(vocabSize);

    for (int i = 0; i < vocabSize; i++) {
        if (std::isnan(logits[i]) || std::isinf(logits[i])) continue;
        candidates.push_back({i, logits[i]});
    }

    if (candidates.empty()) {
        // 极端回退：返回 EOS
        return m_tokenizer->EndOfTextId();
    }

    // Top-K 滤波：只保留 topK 个
    if (candidates.size() > (size_t)m_topK) {
        std::partial_sort(candidates.begin(), candidates.begin() + m_topK, candidates.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
        candidates.resize(m_topK);
    }

    // 应用 temperature + top-P
    ApplyFilters(candidates);

    // 从分布中采样
    float r = g_rng.nextFloat();
    double cumProb = 0.0;
    for (auto& [id, prob] : candidates) {
        cumProb += prob;
        if (r <= cumProb) return id;
    }

    // Fallback: 返回最高概率的 token
    return candidates[0].first;
}

// ============================================================
// 对话模板
// ============================================================

std::string LocalLlmEngine::BuildQwenPrompt(
    const std::vector<std::pair<std::string, std::string>>& messages)
{
    std::string prompt;
    for (const auto& msg : messages) {
        if (msg.first == "system") {
            prompt += "<|im_start|>system\n" + msg.second + "<|im_end|>\n";
        } else if (msg.first == "user") {
            prompt += "<|im_start|>user\n" + msg.second + "<|im_end|>\n";
        } else if (msg.first == "assistant") {
            prompt += "<|im_start|>assistant\n" + msg.second + "<|im_end|>\n";
        }
    }
    prompt += "<|im_start|>assistant\n";
    return prompt;
}

/// 构建带增量上下文的 prompt
/// 如果用户之前打断了 AI 的回答，在 prompt 尾部注入
/// "[AI 正在说: ...]" 上下文，让 LLM 知道之前说到哪了。
std::string LocalLlmEngine::BuildPrompt(
    const std::vector<std::pair<std::string, std::string>>& messages)
{
    std::string prompt = BuildQwenPrompt(messages);

    // 检查是否有未消费的打断上下文
    std::string context;
    {
        std::lock_guard<std::mutex> lock(m_contextMutex);
        if (!m_interruptedPartialText.empty()) {
            context = "[AI 当前回复进度: " + m_interruptedPartialText.substr(0, 200) + "]";
            OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: injecting interrupt context (%{public}zu chars)",
                m_interruptedPartialText.size());
            // 消费后清除
            m_interruptedPartialText.clear();
        }
    }

    if (!context.empty()) {
        // 在最后一个 user 消息后注入上下文
        // <|im_start|>user\n...<|im_end|>\n[AI 当前回复进度: ...]<|im_start|>assistant\n
        static constexpr char kAssistantPrefix[] = "<|im_start|>assistant\n";
        constexpr size_t kAssistantPrefixLength = sizeof(kAssistantPrefix) - 1;
        if (prompt.size() >= kAssistantPrefixLength &&
            prompt.compare(prompt.size() - kAssistantPrefixLength,
                           kAssistantPrefixLength, kAssistantPrefix) == 0) {
            prompt.resize(prompt.size() - kAssistantPrefixLength);
        }
        prompt += context + "\n";
        prompt += kAssistantPrefix;
    }

    return prompt;
}

// ============================================================
// 增量上下文
// ============================================================

void LocalLlmEngine::SaveInterruptedContext() {
    std::lock_guard<std::mutex> lock(m_contextMutex);
    m_interruptedPartialText = m_partialResponse;
    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: saved interrupt context (%{public}zu chars)",
        m_interruptedPartialText.size());
}

std::string LocalLlmEngine::ConsumeInterruptedContext() {
    std::lock_guard<std::mutex> lock(m_contextMutex);
    std::string saved = std::move(m_interruptedPartialText);
    m_interruptedPartialText.clear();
    return saved;
}

// ============================================================
// 流式调用
// ============================================================

void LocalLlmEngine::CallStreaming(
    const std::vector<std::pair<std::string, std::string>>& messages,
    LlmStreamCallback onToken,
    LlmCompleteCallback onComplete)
{
    if (!m_initialized.load()) {
        if (onComplete) onComplete("", false);
        return;
    }

    // 后台线程运行推理。active计数保证内存压力释放时不会销毁正在使用的Session。
    // 使用 CancelScope 替代布尔标志：保存当前 generation，
    // 外部调用 CancelGeneration() 时计数器递增，IsStale 返回 true → 安全退出
    m_activeGenerations.fetch_add(1);
    std::thread([this, messages, onToken = std::move(onToken), onComplete = std::move(onComplete)]() {
        std::lock_guard<std::mutex> lock(m_mutex);

        // 保存当前 generation，用于后续检查是否被中断
        auto gen = m_cancelScope.Get();

        auto startTime = std::chrono::steady_clock::now();

        // 1. 构建 prompt（带增量上下文注入）
        std::string prompt = BuildPrompt(messages);
        OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: prompt len=%{public}zu", prompt.size());

        // 2. 清空部分响应追踪（准备新一次生成）
        {
            std::lock_guard<std::mutex> lock(m_contextMutex);
            m_partialResponse.clear();
        }

        // 3. 编码
        std::vector<int32_t> inputIds = m_tokenizer->EncodeMessages(messages, true);
        OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: input tokens=%{public}zu", inputIds.size());

        // 限制 KV 上下文长度：保留开头系统指令和最近的对话内容。
        if (inputIds.size() > static_cast<size_t>(m_maxContextTokens)) {
            constexpr size_t kKeepPrefixTokens = 64;
            const size_t keepTail = static_cast<size_t>(m_maxContextTokens) - kKeepPrefixTokens;
            std::vector<int32_t> bounded;
            bounded.reserve(m_maxContextTokens);
            bounded.insert(bounded.end(), inputIds.begin(), inputIds.begin() + kKeepPrefixTokens);
            bounded.insert(bounded.end(), inputIds.end() - keepTail, inputIds.end());
            inputIds.swap(bounded);
            OH_LOG_WARN(LOG_APP, "OHOS_LocalLlm: context bounded to %{public}d tokens", m_maxContextTokens);
        }

        // 5. 自回归推理循环
        std::vector<int32_t> outputIds;
        outputIds.reserve(m_maxNewTokens);
        int32_t eosId = m_tokenizer->EndOfTextId();

        auto startInfer = std::chrono::steady_clock::now();
        bool firstTokenRecorded = false;
        auto firstTokenTime = startInfer;
        bool inferenceOk = true;
        std::vector<OrtValue*> kvCache;
        int64_t pastLength = 0;
        std::vector<float> logits;

        // 分块 prefill：每块只产生最多32个位置的 logits，同时逐块建立KV Cache。
        // 相比一次输入512 Token，可将 prefill logits 峰值从约297MB降到约19MB。
        constexpr size_t kPrefillChunkTokens = 32;
        for (size_t offset = 0; offset < inputIds.size() && !m_cancelScope.IsStale(gen);
             offset += kPrefillChunkTokens) {
            const size_t count = std::min(kPrefillChunkTokens, inputIds.size() - offset);
            std::vector<int32_t> chunk(inputIds.begin() + offset, inputIds.begin() + offset + count);
            if (!RunStepWithCache(chunk, logits, kvCache, pastLength)) {
                OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: KV prefill failed at offset %{public}zu", offset);
                inferenceOk = false;
                break;
            }
        }

        for (int step = 0; inferenceOk && step < m_maxNewTokens &&
             !m_cancelScope.IsStale(gen); step++) {
            // 采样
            int32_t nextToken = SampleToken(logits.data(), (int)logits.size());

            // 检查 EOS
            if (nextToken == eosId ||
                nextToken == m_tokenizer->ImEndId()) {
                break;
            }

            // 记录输出
            outputIds.push_back(nextToken);
            if (!firstTokenRecorded) {
                firstTokenTime = std::chrono::steady_clock::now();
                const double firstTokenMs =
                    std::chrono::duration_cast<std::chrono::microseconds>(firstTokenTime - startInfer).count() / 1000.0;
                MetricsCollector::RecordLlmFirstToken(firstTokenMs);
                firstTokenRecorded = true;
            }

            // 流式回调
            std::string tokenStr = m_tokenizer->DecodeToken(nextToken);
            if (!tokenStr.empty()) {
                // 追踪部分响应（用于增量上下文）
                {
                    std::lock_guard<std::mutex> lock(m_contextMutex);
                    m_partialResponse += tokenStr;
                }
                if (!onToken(tokenStr)) {
                    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: stopped by request");
                    break;
                }
            }

            // 每 10 步打印一次性能日志
            if (step % 10 == 0 && step > 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startInfer).count();
                OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: step=%{public}d/%{public}d, %{public}lldms, tok/s=%.1f",
                    step, m_maxNewTokens, (long long)elapsed, (step * 1000.0f) / (elapsed + 1));
            }

            // 后续步骤只输入刚生成的1个Token，历史上下文全部来自KV Cache。
            if (step + 1 < m_maxNewTokens && !m_cancelScope.IsStale(gen)) {
                const std::vector<int32_t> oneToken{nextToken};
                if (!RunStepWithCache(oneToken, logits, kvCache, pastLength)) {
                    OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: cached decode failed at step %{public}d", step);
                    inferenceOk = false;
                    break;
                }
            }
        }
        ReleaseKvCache(kvCache);

        auto endTime = std::chrono::steady_clock::now();
        auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
        OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: generated %{public}zu tokens in %{public}lldms (%.1f tok/s)",
            outputIds.size(), (long long)totalMs,
            outputIds.size() * 1000.0f / (totalMs + 1));
        const double decodeMs = firstTokenRecorded
            ? std::chrono::duration_cast<std::chrono::microseconds>(endTime - firstTokenTime).count() / 1000.0
            : 0.0;
        const double tokensPerSec = outputIds.size() > 1
            ? (outputIds.size() - 1) * 1000.0 / (decodeMs + 1.0)
            : 0.0;
        MetricsCollector::RecordLlmTokensPerSec(tokensPerSec);
        MetricsCollector::RecordProcessMemorySnapshot();

        // 6. 解码完整输出
        std::string fullText = outputIds.empty() ? "" : m_tokenizer->Decode(outputIds);

        if (onComplete) onComplete(fullText, inferenceOk && !outputIds.empty());
        m_activeGenerations.fetch_sub(1);
        m_activeCv.notify_all();
    }).detach();
}

// ============================================================
// 同步调用（阻塞等待完整结果）
// ============================================================

std::string LocalLlmEngine::Call(
    const std::vector<std::pair<std::string, std::string>>& messages)
{
    if (!m_initialized.load()) return "";
    return "Use CallStreaming for local LLM (blocking Call not fully supported)";
}
