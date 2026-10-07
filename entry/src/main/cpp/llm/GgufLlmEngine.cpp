#include "GgufLlmEngine.h"

#include "ggml-backend.h"
#include "llama.h"
#include "metrics/MetricsCollector.h"

#include <hilog/log.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#if defined(OHOS_GGML_CPU_VARIANTS)
#include <dlfcn.h>
#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif
#endif

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "OHOS_GgufLlm"

namespace {

bool FileExists(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    fclose(file);
    return true;
}

#if defined(OHOS_GGML_CPU_VARIANTS)
std::string NativeLibraryDirectory() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&NativeLibraryDirectory), &info) == 0 ||
        info.dli_fname == nullptr || info.dli_fname[0] == '\0') {
        return {};
    }
    std::string path(info.dli_fname);
    const size_t separator = path.find_last_of("/\\");
    return separator == std::string::npos ? std::string{} : path.substr(0, separator);
}

struct ArmCpuFeatures {
    unsigned long hwcap = 0;
    bool dotprod = false;
    bool fp16 = false;
};

struct CpuBackendCandidate {
    const char* filename;
    int tier;
};

int g_selectedCpuBackendTier = -1;

ArmCpuFeatures DetectArmCpuFeatures() {
    ArmCpuFeatures features;
#if defined(__aarch64__) && defined(__linux__)
    features.hwcap = getauxval(AT_HWCAP);
    features.dotprod = (features.hwcap & HWCAP_ASIMDDP) != 0;
    features.fp16 = (features.hwcap & HWCAP_FPHP) != 0;
#endif
    return features;
}

bool HasRegisteredCpuBackend() {
    for (size_t index = 0; index < ggml_backend_reg_count(); ++index) {
        ggml_backend_reg_t backend = ggml_backend_reg_get(index);
        const char* name = backend ? ggml_backend_reg_name(backend) : nullptr;
        if (name != nullptr && std::strcmp(name, "CPU") == 0) {
            return true;
        }
    }
    return false;
}

double ElapsedMs(std::chrono::steady_clock::time_point startedAt) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startedAt).count();
}

bool LoadDynamicCpuBackend() {
    const auto selectionStartedAt = std::chrono::steady_clock::now();
    const ArmCpuFeatures features = DetectArmCpuFeatures();
    OH_LOG_INFO(LOG_APP,
        "GGML CPU variants: HWCAP=0x%{public}lx dotprod=%{public}d fp16=%{public}d",
        features.hwcap, features.dotprod ? 1 : 0, features.fp16 ? 1 : 0);
    MetricsCollector::Record(MetricType::CpuDotprod, features.dotprod ? 1.0 : 0.0);
    MetricsCollector::Record(MetricType::CpuFp16, features.fp16 ? 1.0 : 0.0);

    const std::string nativeLibraryDir = NativeLibraryDirectory();
    if (nativeLibraryDir.empty()) {
        OH_LOG_ERROR(LOG_APP, "GGML CPU variants: failed to resolve native library directory");
        return false;
    }

    if (HasRegisteredCpuBackend()) {
        const double selectMs = ElapsedMs(selectionStartedAt);
        MetricsCollector::Record(MetricType::LlmBackendSelectMs, selectMs);
        MetricsCollector::Record(MetricType::LlmBackendLoadMs, 0.0);
        if (g_selectedCpuBackendTier >= 0) {
            MetricsCollector::Record(
                MetricType::LlmBackendTier, static_cast<double>(g_selectedCpuBackendTier));
        }
        OH_LOG_INFO(LOG_APP,
            "GGML CPU variants: reuse registered backend tier=%{public}d select_ms=%{public}.3f",
            g_selectedCpuBackendTier, selectMs);
        return true;
    }

    std::vector<CpuBackendCandidate> candidates;
    if (features.dotprod && features.fp16) {
        candidates.push_back({"libggml-cpu-armv8.2_2.so", 2});
    }
    if (features.dotprod) {
        candidates.push_back({"libggml-cpu-armv8.2_1.so", 1});
    }
    candidates.push_back({"libggml-cpu-armv8.0_1.so", 0});

    const double selectMs = ElapsedMs(selectionStartedAt);
    MetricsCollector::Record(MetricType::LlmBackendSelectMs, selectMs);

    const auto loadStartedAt = std::chrono::steady_clock::now();
    for (const CpuBackendCandidate& candidate : candidates) {
        const std::string path = nativeLibraryDir + "/" + candidate.filename;
        if (!FileExists(path)) {
            OH_LOG_WARN(LOG_APP,
                "GGML CPU variants: candidate missing tier=%{public}d path=%{public}s",
                candidate.tier, path.c_str());
            continue;
        }

        OH_LOG_INFO(LOG_APP,
            "GGML CPU variants: direct load tier=%{public}d path=%{public}s",
            candidate.tier, path.c_str());
        ggml_backend_reg_t backend = ggml_backend_load(path.c_str());
        if (backend != nullptr) {
            g_selectedCpuBackendTier = candidate.tier;
            const double loadMs = ElapsedMs(loadStartedAt);
            MetricsCollector::Record(MetricType::LlmBackendLoadMs, loadMs);
            MetricsCollector::Record(
                MetricType::LlmBackendTier, static_cast<double>(candidate.tier));
            OH_LOG_INFO(LOG_APP,
                "GGML CPU variants: selected tier=%{public}d file=%{public}s "
                "select_ms=%{public}.3f load_ms=%{public}.3f",
                candidate.tier, candidate.filename, selectMs, loadMs);
            return true;
        }

        OH_LOG_WARN(LOG_APP,
            "GGML CPU variants: load failed, falling back from tier=%{public}d",
            candidate.tier);
    }

    const double loadMs = ElapsedMs(loadStartedAt);
    MetricsCollector::Record(MetricType::LlmBackendLoadMs, loadMs);
    OH_LOG_INFO(LOG_APP,
        "GGML CPU variants: no compatible backend select_ms=%{public}.3f load_ms=%{public}.3f",
        selectMs, loadMs);
    return false;
}
#endif

std::string QwenFallbackPrompt(
    const std::vector<std::pair<std::string, std::string>>& messages,
    const std::string& systemPrompt) {
    std::string result;
    bool hasSystem = false;
    for (const auto& message : messages) {
        if (message.first == "system") hasSystem = true;
    }
    if (!hasSystem && !systemPrompt.empty()) {
        result += "<|im_start|>system\n" + systemPrompt + "<|im_end|>\n";
    }
    for (const auto& message : messages) {
        result += "<|im_start|>" + message.first + "\n" + message.second + "<|im_end|>\n";
    }
    result += "<|im_start|>assistant\n";
    return result;
}

size_t CompleteUtf8PrefixLength(const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        size_t width = 1;
        if ((lead & 0x80U) == 0) {
            width = 1;
        } else if ((lead & 0xE0U) == 0xC0U) {
            width = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            width = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            width = 4;
        } else {
            ++offset;
            continue;
        }
        if (offset + width > text.size()) break;
        bool valid = true;
        for (size_t i = 1; i < width; ++i) {
            if ((static_cast<unsigned char>(text[offset + i]) & 0xC0U) != 0x80U) {
                valid = false;
                break;
            }
        }
        offset += valid ? width : 1;
    }
    return offset;
}

}

GgufLlmEngine::GgufLlmEngine() {
    OH_LOG_INFO(LOG_APP, "GGUF LLM created");
}

GgufLlmEngine::~GgufLlmEngine() {
    GgufLlmEngine::Release();
}

bool GgufLlmEngine::Init(const LlmConfig& config) {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    if (m_initialized.load()) return true;
    if (!FileExists(config.modelPath)) {
        OH_LOG_ERROR(LOG_APP, "GGUF model missing: %{public}s", config.modelPath.c_str());
        return false;
    }

    m_config = config;
    m_maxNewTokens = std::max(1, std::min(config.maxTokens, 512));
    llama_log_set([](enum ggml_log_level level, const char* text, void*) {
        if (level == GGML_LOG_LEVEL_ERROR) {
            OH_LOG_ERROR(LOG_APP, "llama.cpp: %{public}s", text ? text : "unknown error");
#if defined(OHOS_GGML_CPU_VARIANTS)
        } else if (level == GGML_LOG_LEVEL_INFO && text != nullptr &&
                   std::strstr(text, "ggml_backend_load_best: selected") != nullptr) {
            OH_LOG_INFO(LOG_APP, "llama.cpp: %{public}s", text);
#endif
        }
    }, nullptr);
#if defined(OHOS_GGML_CPU_VARIANTS)
    if (!LoadDynamicCpuBackend()) {
        OH_LOG_ERROR(LOG_APP, "No compatible GGML CPU backend was loaded");
        return false;
    }
#endif
    llama_backend_init();
    m_backendInitialized = true;

    llama_model_params modelParams = llama_model_default_params();
    modelParams.n_gpu_layers = 0;
    modelParams.use_mmap = true;
    modelParams.use_mlock = false;
    modelParams.use_extra_bufts = false;
    const auto modelLoadStartedAt = std::chrono::steady_clock::now();
    m_model = llama_model_load_from_file(config.modelPath.c_str(), modelParams);
    const double modelLoadMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - modelLoadStartedAt).count();
    MetricsCollector::Record(MetricType::LlmModelLoadMs, modelLoadMs);
    OH_LOG_INFO(LOG_APP, "GGUF model load: %{public}.3f ms", modelLoadMs);
    if (!m_model) {
        OH_LOG_ERROR(LOG_APP, "Failed to load GGUF model");
        llama_backend_free();
        m_backendInitialized = false;
        return false;
    }

    llama_context_params contextParams = llama_context_default_params();
    contextParams.n_ctx = static_cast<uint32_t>(m_contextTokens);
    contextParams.n_batch = static_cast<uint32_t>(m_batchTokens);
    contextParams.n_ubatch = static_cast<uint32_t>(m_batchTokens);
    contextParams.n_threads = m_threads;
    contextParams.n_threads_batch = m_threads;
    contextParams.type_k = GGML_TYPE_Q8_0;
    contextParams.type_v = GGML_TYPE_Q8_0;
    contextParams.offload_kqv = false;
    contextParams.no_perf = false;
    contextParams.abort_callback = AbortCallback;
    contextParams.abort_callback_data = &m_abortRequested;
    const auto contextInitStartedAt = std::chrono::steady_clock::now();
    m_context = llama_init_from_model(m_model, contextParams);
    if (!m_context) {
        OH_LOG_WARN(LOG_APP, "Q8 KV cache unavailable, retrying with default cache types");
        contextParams.type_k = GGML_TYPE_F16;
        contextParams.type_v = GGML_TYPE_F16;
        m_context = llama_init_from_model(m_model, contextParams);
    }
    const double contextInitMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - contextInitStartedAt).count();
    MetricsCollector::Record(MetricType::LlmContextInitMs, contextInitMs);
    OH_LOG_INFO(LOG_APP, "GGUF context init: %{public}.3f ms", contextInitMs);
    if (!m_context) {
        OH_LOG_ERROR(LOG_APP, "Failed to create GGUF context");
        llama_model_free(m_model);
        m_model = nullptr;
        llama_backend_free();
        m_backendInitialized = false;
        return false;
    }

    m_vocab = llama_model_get_vocab(m_model);
    m_abortRequested.store(false);
    m_initialized.store(true);
    OH_LOG_INFO(LOG_APP,
        "GGUF initialized: ctx=%{public}d batch=%{public}d threads=%{public}d mmap=1",
        m_contextTokens, m_batchTokens, m_threads);
    OH_LOG_INFO(LOG_APP, "llama.cpp CPU backend: %{public}s", llama_print_system_info());
    MetricsCollector::RecordProcessMemorySnapshot();
    return true;
}

void GgufLlmEngine::Release() {
    CancelGeneration();
    {
        std::unique_lock<std::mutex> activeLock(m_activeMutex);
        m_activeCv.wait(activeLock, [this]() { return m_activeGenerations.load() == 0; });
    }
    std::lock_guard<std::mutex> lock(m_engineMutex);
    m_initialized.store(false);
    if (m_context) {
        llama_free(m_context);
        m_context = nullptr;
    }
    m_vocab = nullptr;
    if (m_model) {
        llama_model_free(m_model);
        m_model = nullptr;
    }
    if (m_backendInitialized) {
        llama_backend_free();
        m_backendInitialized = false;
    }
}

void GgufLlmEngine::CancelGeneration() {
    m_abortRequested.store(true);
}

void GgufLlmEngine::SaveInterruptedContext() {
    std::lock_guard<std::mutex> lock(m_contextMutex);
    m_interruptedPartialText = m_partialResponse;
}

void GgufLlmEngine::SetDiagnosticDecodeTokens(int tokens) {
    m_diagnosticDecodeTokens.store(std::max(0, std::min(tokens, 256)));
}

bool GgufLlmEngine::AbortCallback(void* data) {
    return static_cast<std::atomic<bool>*>(data)->load();
}

void GgufLlmEngine::GenerationFinished() {
    m_activeGenerations.fetch_sub(1);
    m_activeCv.notify_all();
}

std::string GgufLlmEngine::BuildPrompt(
    const std::vector<std::pair<std::string, std::string>>& messages) {
    std::vector<std::pair<std::string, std::string>> effectiveMessages = messages;
    bool hasSystem = false;
    for (const auto& message : effectiveMessages) {
        if (message.first == "system") hasSystem = true;
    }
    if (!hasSystem && !m_config.systemPrompt.empty()) {
        effectiveMessages.insert(effectiveMessages.begin(), {"system", m_config.systemPrompt});
    }

    std::string interruptContext;
    {
        std::lock_guard<std::mutex> lock(m_contextMutex);
        if (!m_interruptedPartialText.empty()) {
            interruptContext = m_interruptedPartialText.substr(0, 200);
            m_interruptedPartialText.clear();
        }
    }
    if (!interruptContext.empty()) {
        effectiveMessages.push_back({"system", "The previous answer was interrupted after: " + interruptContext});
    }

    std::vector<llama_chat_message> chat;
    chat.reserve(effectiveMessages.size());
    for (const auto& message : effectiveMessages) {
        chat.push_back({message.first.c_str(), message.second.c_str()});
    }
    const char* chatTemplate = llama_model_chat_template(m_model, nullptr);
    int32_t length = llama_chat_apply_template(
        chatTemplate, chat.data(), chat.size(), true, nullptr, 0);
    if (length <= 0) return QwenFallbackPrompt(effectiveMessages, "");
    std::vector<char> formatted(static_cast<size_t>(length));
    int32_t written = llama_chat_apply_template(
        chatTemplate, chat.data(), chat.size(), true, formatted.data(), length);
    if (written < 0) return QwenFallbackPrompt(effectiveMessages, "");
    return std::string(formatted.data(), static_cast<size_t>(written));
}

bool GgufLlmEngine::Tokenize(const std::string& text, std::vector<int>& tokens) const {
    int32_t count = llama_tokenize(m_vocab, text.c_str(), text.size(), nullptr, 0, true, true);
    if (count >= 0) return false;
    tokens.resize(static_cast<size_t>(-count));
    count = llama_tokenize(m_vocab, text.c_str(), text.size(),
        reinterpret_cast<llama_token*>(tokens.data()), static_cast<int32_t>(tokens.size()), true, true);
    if (count < 0) return false;
    tokens.resize(static_cast<size_t>(count));
    return true;
}

std::string GgufLlmEngine::TokenToPiece(int token) const {
    char small[256];
    int32_t count = llama_token_to_piece(m_vocab, token, small, sizeof(small), 0, true);
    if (count >= 0) return std::string(small, static_cast<size_t>(count));
    std::vector<char> buffer(static_cast<size_t>(-count));
    count = llama_token_to_piece(m_vocab, token, buffer.data(), buffer.size(), 0, true);
    return count > 0 ? std::string(buffer.data(), static_cast<size_t>(count)) : std::string();
}

void GgufLlmEngine::CallStreaming(
    const std::vector<std::pair<std::string, std::string>>& messages,
    LlmStreamCallback onToken,
    LlmCompleteCallback onComplete) {
    if (!m_initialized.load()) {
        if (onComplete) onComplete("", false);
        return;
    }

    m_activeGenerations.fetch_add(1);
    std::thread([this, messages, onToken = std::move(onToken), onComplete = std::move(onComplete)]() mutable {
        std::lock_guard<std::mutex> engineLock(m_engineMutex);
        if (!m_initialized.load() || !m_context || !m_vocab) {
            if (onComplete) onComplete("", false);
            GenerationFinished();
            return;
        }

        m_abortRequested.store(false);
        llama_memory_clear(llama_get_memory(m_context), true);
        const int requestedDiagnosticSteps = m_diagnosticDecodeTokens.exchange(0);
        {
            std::lock_guard<std::mutex> lock(m_contextMutex);
            m_partialResponse.clear();
        }

        const auto start = std::chrono::steady_clock::now();
        std::vector<int> promptTokens;
        const std::string prompt = BuildPrompt(messages);
        if (!Tokenize(prompt, promptTokens) || promptTokens.empty()) {
            OH_LOG_ERROR(LOG_APP, "GGUF prompt tokenization failed");
            if (onComplete) onComplete("", false);
            GenerationFinished();
            return;
        }

        const size_t maxPromptTokens = static_cast<size_t>(
            std::max(128, m_contextTokens - m_maxNewTokens));
        if (promptTokens.size() > maxPromptTokens) {
            OH_LOG_WARN(LOG_APP,
                "GGUF prompt exceeded complete-turn budget, applying safety trim: tokens=%{public}zu max=%{public}zu",
                promptTokens.size(), maxPromptTokens);
            constexpr size_t keepPrefix = 64;
            std::vector<int> bounded;
            bounded.reserve(maxPromptTokens);
            bounded.insert(bounded.end(), promptTokens.begin(), promptTokens.begin() + keepPrefix);
            bounded.insert(bounded.end(), promptTokens.end() - (maxPromptTokens - keepPrefix), promptTokens.end());
            promptTokens.swap(bounded);
        }

        bool inferenceOk = true;
        const auto prefillStart = std::chrono::steady_clock::now();
        for (size_t offset = 0; offset < promptTokens.size() && !m_abortRequested.load();
             offset += static_cast<size_t>(m_batchTokens)) {
            const size_t count = std::min(static_cast<size_t>(m_batchTokens), promptTokens.size() - offset);
            llama_batch batch = llama_batch_get_one(
                reinterpret_cast<llama_token*>(promptTokens.data() + offset), static_cast<int32_t>(count));
            const int result = llama_decode(m_context, batch);
            if (result != 0) {
                if (!m_abortRequested.load()) {
                    OH_LOG_ERROR(LOG_APP, "GGUF prefill failed: %{public}d", result);
                    inferenceOk = false;
                }
                break;
            }
        }

        const auto prefillEnd = std::chrono::steady_clock::now();
        const double prefillMs = std::chrono::duration_cast<std::chrono::microseconds>(
            prefillEnd - prefillStart).count() / 1000.0;
        MetricsCollector::Record(
            MetricType::LlmPromptTokens, static_cast<double>(promptTokens.size()));
        MetricsCollector::Record(MetricType::LlmPrefillMs, prefillMs);

        llama_token diagnosticToken = LLAMA_TOKEN_NULL;
        if (requestedDiagnosticSteps > 0) {
            std::vector<int> diagnosticTokens;
            if (Tokenize(" benchmark", diagnosticTokens)) {
                for (auto it = diagnosticTokens.rbegin(); it != diagnosticTokens.rend(); ++it) {
                    if (!llama_vocab_is_eog(m_vocab, *it)) {
                        diagnosticToken = static_cast<llama_token>(*it);
                        break;
                    }
                }
            }
            OH_LOG_INFO(LOG_APP,
                "GGUF diagnostic decode: requested=%{public}d token=%{public}d",
                requestedDiagnosticSteps, diagnosticToken);
        }

        llama_sampler* sampler = nullptr;
        if (diagnosticToken == LLAMA_TOKEN_NULL && inferenceOk && !m_abortRequested.load()) {
            sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
            llama_sampler_chain_add(sampler, llama_sampler_init_top_k(40));
            llama_sampler_chain_add(sampler, llama_sampler_init_top_p(0.9f, 1));
            // Suppress short self-reinforcing loops without materially increasing
            // decode cost. Apply after top-k/top-p as recommended by llama.cpp.
            llama_sampler_chain_add(sampler, llama_sampler_init_penalties(64, 1.10f, 0.0f, 0.0f));
            llama_sampler_chain_add(sampler, llama_sampler_init_temp(std::max(0.05f, m_config.temperature)));
            llama_sampler_chain_add(sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
        }

        std::string fullText;
        std::string pendingUtf8;
        bool firstTokenRecorded = false;
        auto firstTokenTime = start;
        const int available = m_contextTokens - static_cast<int>(promptTokens.size());
        const int requestedLimit = diagnosticToken != LLAMA_TOKEN_NULL
            ? requestedDiagnosticSteps : m_maxNewTokens;
        const int generationLimit = std::max(0, std::min(requestedLimit, available));
        int generatedTokens = 0;
        for (int step = 0;
             (sampler || diagnosticToken != LLAMA_TOKEN_NULL) &&
             step < generationLimit && !m_abortRequested.load(); ++step) {
            llama_token token = diagnosticToken != LLAMA_TOKEN_NULL
                ? diagnosticToken : llama_sampler_sample(sampler, m_context, -1);
            if (diagnosticToken == LLAMA_TOKEN_NULL && llama_vocab_is_eog(m_vocab, token)) break;
            ++generatedTokens;

            const std::string piece = TokenToPiece(token);
            if (!piece.empty()) {
                pendingUtf8 += piece;
                const size_t completeBytes = CompleteUtf8PrefixLength(pendingUtf8);
                const std::string visiblePiece = pendingUtf8.substr(0, completeBytes);
                pendingUtf8.erase(0, completeBytes);
                if (!firstTokenRecorded) {
                    firstTokenTime = std::chrono::steady_clock::now();
                    const double firstTokenMs = std::chrono::duration_cast<std::chrono::microseconds>(
                        firstTokenTime - start).count() / 1000.0;
                    MetricsCollector::RecordLlmFirstToken(firstTokenMs);
                    firstTokenRecorded = true;
                }
                if (!visiblePiece.empty()) {
                    fullText += visiblePiece;
                    {
                        std::lock_guard<std::mutex> lock(m_contextMutex);
                        // Keep the interrupt snapshot incremental. Assigning
                        // fullText here copied the whole response once per
                        // token and caused avoidable quadratic memory traffic.
                        m_partialResponse += visiblePiece;
                    }
                    if (onToken && !onToken(visiblePiece)) break;
                }
            }

            llama_batch batch = llama_batch_get_one(&token, 1);
            const int result = llama_decode(m_context, batch);
            if (result != 0) {
                if (!m_abortRequested.load()) {
                    OH_LOG_ERROR(LOG_APP, "GGUF decode failed: %{public}d", result);
                    inferenceOk = false;
                }
                break;
            }
        }
        if (sampler) llama_sampler_free(sampler);

        const auto end = std::chrono::steady_clock::now();
        const double decodeMs = firstTokenRecorded
            ? std::chrono::duration_cast<std::chrono::microseconds>(end - firstTokenTime).count() / 1000.0
            : 0.0;
        const double generationMs = std::chrono::duration_cast<std::chrono::microseconds>(
            end - prefillEnd).count() / 1000.0;
        MetricsCollector::Record(
            MetricType::LlmGeneratedTokens, static_cast<double>(generatedTokens));
        MetricsCollector::Record(MetricType::LlmDecodeMs, generationMs);
        MetricsCollector::RecordLlmTokensPerSec(
            decodeMs > 0.0 && generatedTokens > 1 ? (generatedTokens - 1) * 1000.0 / decodeMs : 0.0);
        MetricsCollector::RecordProcessMemorySnapshot();
        OH_LOG_INFO(LOG_APP, "GGUF generation complete: bytes=%{public}zu cancelled=%{public}d",
            fullText.size(), m_abortRequested.load() ? 1 : 0);

        const bool completed = inferenceOk && !m_abortRequested.load() && !fullText.empty();
        if (onComplete) onComplete(fullText, completed);
        GenerationFinished();
    }).detach();
}

std::string GgufLlmEngine::Call(
    const std::vector<std::pair<std::string, std::string>>&) {
    return "Use CallStreaming for local GGUF LLM";
}
