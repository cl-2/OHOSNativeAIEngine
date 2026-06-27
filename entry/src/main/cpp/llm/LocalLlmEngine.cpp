#include "LocalLlmEngine.h"
#include "onnxruntime/onnxruntime_c_api.h"
#include <hilog/log.h>
#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cmath>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "OHOS_LocalLlm"

// ============================================================
// LocalLlmEngine — 基于 ONNX Runtime 的 Qwen2.5-0.5B 推理
// 
// 模型文件（由用户放入沙箱目录）：
//   model.int4.onnx   ~350MB  INT4 量化后的 Qwen2.5-0.5B
//   tokenizer.json     HuggingFace 格式分词器
//   config.json        模型配置
//
// 下载模型：huggingface.co/Qwen/Qwen2.5-0.5B-Instruct
// 转换为 ONNX：optimum-cli export onnx --model Qwen/Qwen2.5-0.5B-Instruct --quantize int4
// ============================================================

LocalLlmEngine::LocalLlmEngine() {
    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: created");
}

LocalLlmEngine::~LocalLlmEngine() {
    Release();
}

void LocalLlmEngine::Log(const char* msg) {
    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: %{public}s", msg);
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

    // 加载 tokenizer.json
    if (!LoadTokenizer(m_tokenizerPath)) {
        return false;
    }

    // 检查模型文件是否存在
    FILE* f = fopen(m_modelPath.c_str(), "rb");
    if (!f) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: model not found at %{public}s", m_modelPath.c_str());
        return false;
    }
    fclose(f);
    Log("Model file exists, O Kallos Runtime will load it");

    m_initialized.store(true);
    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: Init done");
    return true;
}

void LocalLlmEngine::Release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_initialized.store(false);
    Log("Released");
}

// ============================================================
// 分词器
// ============================================================

bool LocalLlmEngine::LoadTokenizer(const std::string& tokenizerPath) {
    std::ifstream in(tokenizerPath, std::ios::binary);
    if (!in.is_open()) {
        OH_LOG_ERROR(LOG_APP, "OHOS_LocalLlm: cannot open tokenizer: %{public}s", tokenizerPath.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    m_tokenizerJson = ss.str();
    OH_LOG_INFO(LOG_APP, "OHOS_LocalLlm: tokenizer loaded (%{public}zu bytes)", m_tokenizerJson.size());
    return true;
}

std::vector<int32_t> LocalLlmEngine::Encode(const std::string& text) {
    // Qwen2.5 使用 tiktoken BPE 编码
    // 完整实现需要解析 tokenizer.json 中的 merges 和 vocab
    // 简化版本：将每个 UTF-8 字节映射到 token ID 范围
    std::vector<int32_t> ids;

    // Qwen2.5 chat template special tokens
    ids.push_back(151644); // <|im_start|>

    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = (unsigned char)text[i];
        if (c & 0x80) { // UTF-8 multi-byte
            int len = 1;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            std::string ch = text.substr(i, len);
            int id = 151936 + (int)((unsigned char)ch[0] & 0x7F) * 4 + (len > 1 ? (int)((unsigned char)ch[1] & 0x3F) / 64 : 0);
            id = std::min(id, 152000);
            ids.push_back(id);
            i += len;
        } else if (c >= 'a' && c <= 'z') {
            ids.push_back(151936 + 128 + (c - 'a'));
            i++;
        } else if (c >= 'A' && c <= 'Z') {
            ids.push_back(151936 + 128 + 26 + (c - 'A'));
            i++;
        } else if (c >= '0' && c <= '9') {
            ids.push_back(151936 + 128 + 52 + (c - '0'));
            i++;
        } else if (c == ' ') {
            ids.push_back(151937);
            i++;
        } else if (c == '\n') {
            ids.push_back(151938);
            i++;
        } else {
            ids.push_back(151936 + 128 + 62 + c);
            i++;
        }
    }
    ids.push_back(151645); // <|im_end|>
    return ids;
}

std::string LocalLlmEngine::Decode(const std::vector<int32_t>& ids) {
    std::string result;
    for (int32_t id : ids) {
        if (id == 151644 || id == 151645 || id == 151646) continue; // special tokens
        if (id == 0 || id == 1 || id == 2) continue; // pad/bos/eos
        if (id == 151937) { result += ' '; continue; }
        if (id == 151938) { result += '\n'; continue; }
        if (id >= 151936 && id <= 152000) {
            result += '?'; // placeholder for decoded text
        } else if (id >= 3 && id < 259) {
            result += (char)(id - 3);
        }
    }
    return result;
}

// ============================================================
// 对话模板
// ============================================================

std::string LocalLlmEngine::BuildPrompt(const std::vector<std::pair<std::string, std::string>>& messages) {
    // Qwen2.5 chat template
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

// ============================================================
// 推理
// ============================================================

bool LocalLlmEngine::RunInference(const std::vector<int32_t>& inputIds,
                                   std::vector<int32_t>& outputIds,
                                   int maxNewTokens) {
    // ONNX Runtime 推理循环
    // 1. 使用 OrtApi 创建 Session、构建输入 tensor、Run、采样
    // 2. 重复采样直到 EOS 或达到 maxNewTokens
    //
    // TODO: 完整推理循环代码（模型就位后实现）
    // 当前返回占位文本以供测试
    outputIds.clear();
    const char* placeholder = "（本地模型加载成功，ONNX 推理循环待实现）";
    for (const char* p = placeholder; *p; p++) {
        outputIds.push_back(151936 + 128 + (unsigned char)*p % 62);
    }
    return true;
}

// ============================================================
// 公开接口
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

    m_generating.store(true);

    // 在后台线程中运行推理
    std::thread([this, messages, onToken = std::move(onToken), onComplete = std::move(onComplete)]() {
        std::lock_guard<std::mutex> lock(m_mutex);

        // 构建 prompt
        std::string prompt = BuildPrompt(messages);
        
        // 编码
        std::vector<int32_t> inputIds = Encode(prompt);
        
        // 推理
        std::vector<int32_t> outputIds;
        if (!RunInference(inputIds, outputIds, m_config.maxTokens)) {
            if (onComplete) onComplete("", false);
            m_generating.store(false);
            return;
        }

        // 解码并流式回调
        std::string fullText;
        std::string accumulated;
        for (size_t i = 0; i < outputIds.size(); i++) {
            if (!m_generating.load()) break;
            std::vector<int32_t> singleId = {outputIds[i]};
            std::string token = Decode(singleId);
            accumulated += token;
            
            // 每积累一段回调一次
            if (accumulated.size() >= 4 || i == outputIds.size() - 1) {
                if (onToken && !onToken(accumulated)) {
                    break; // 外部请求停止
                }
                accumulated.clear();
            }
        }
        fullText = Decode(outputIds);

        if (onComplete) onComplete(fullText, true);
        m_generating.store(false);
    }).detach();
}

std::string LocalLlmEngine::Call(
    const std::vector<std::pair<std::string, std::string>>& messages)
{
    // TODO: 等待流式结果完成，收集完整文本返回
    // 当前返回空字符串，流式版本是主要接口
    return "";
}
