#include "SupertonicTtsEngine.h"
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <vector>
#include <fstream>
#include <dlfcn.h>
#include <hilog/log.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "SupertonicTTS"
#define LOG_DOMAIN 0x0400

// ============================================================
// ONNX Runtime 动态加载 (通过 dlopen 避免编译时链接)
// libonnxruntime.so 已存在于 entry/libs/{arch}/
// ============================================================

// ONNX Runtime C API 函数指针类型
typedef struct OrtApi OrtApi;
typedef struct OrtSession OrtSession;
typedef struct OrtEnv OrtEnv;
typedef struct OrtMemoryInfo OrtMemoryInfo;
typedef struct OrtValue OrtValue;
typedef struct OrtRunOptions OrtRunOptions;
typedef struct OrtSessionOptions OrtSessionOptions;

// 全局 ORT API 指针
static const OrtApi* g_ortApi = nullptr;
static void* g_ortLibHandle = nullptr;

// ONNX Runtime API 函数指针
using OrtCreateEnv_t = OrtEnv* (*)(const char*, int, void**);
using OrtCreateSession_t = OrtSession* (*)(OrtEnv*, const char*, OrtSessionOptions*);
using OrtRun_t = void (*)(OrtSession*, OrtRunOptions*, const char**, 
                          OrtValue**, size_t, const char**, size_t*, 
                          OrtValue**, size_t);

// ORT C API 函数类型
using OrtGetApiBase_t = const OrtApi* (*)();

static OrtGetApiBase_t g_OrtGetApiBase = nullptr;
static bool g_ortLoaded = false;

/**
 * @brief 动态加载 ONNX Runtime 库
 */
static bool LoadOnnxRuntime() {
    if (g_ortLoaded) return true;

    // 尝试加载 libonnxruntime.so
    const char* libPaths[] = {
        "libonnxruntime.so.1.18.0",
        "libonnxruntime.so",
        "libonnxruntime.so.1",
    };

    for (auto path : libPaths) {
        g_ortLibHandle = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
        if (g_ortLibHandle) {
            OH_LOG_INFO(LOG_APP, "SupertonicTTS: loaded %{public}s", path);
            break;
        }
        OH_LOG_WARN(LOG_APP, "SupertonicTTS: dlopen(%{public}s) failed: %{public}s", 
                    path, dlerror());
    }

    if (!g_ortLibHandle) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: cannot load libonnxruntime.so");
        return false;
    }

    // 获取 OrtGetApiBase
    g_OrtGetApiBase = (OrtGetApiBase_t)dlsym(g_ortLibHandle, "OrtGetApiBase");
    if (!g_OrtGetApiBase) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: dlsym OrtGetApiBase failed");
        dlclose(g_ortLibHandle);
        g_ortLibHandle = nullptr;
        return false;
    }

    g_ortApi = g_OrtGetApiBase();
    if (!g_ortApi) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: OrtGetApiBase returned null");
        dlclose(g_ortLibHandle);
        g_ortLibHandle = nullptr;
        return false;
    }

    g_ortLoaded = true;
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: ONNX Runtime loaded successfully");
    return true;
}

// ============================================================
// SupertonicTtsEngine 实现
// ============================================================

SupertonicTtsEngine::SupertonicTtsEngine() {
}

SupertonicTtsEngine::~SupertonicTtsEngine() {
    Release();
}

bool SupertonicTtsEngine::Init(const SupertonicTtsConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized) {
        OH_LOG_WARN(LOG_APP, "SupertonicTTS: already initialized, re-initializing");
        Release();
    }

    m_config = config;
    m_interrupted.store(false);

    // 1. 加载 ONNX Runtime
    if (!LoadOnnxRuntime()) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: ONNX Runtime load failed");
        return false;
    }

    // 2. 确定模型文件目录：先在 modelDir 下直接找，再试 modelDir/onnx/
    std::string searchDir = config.modelDir;
    std::string textEncoderPath = searchDir + "/text_encoder.onnx";
    
    // 如果直接目录找不到，尝试 onnx/ 子目录
    auto checkFile = [](const std::string& path) -> bool {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fclose(f);
        return true;
    };
    
    if (!checkFile(textEncoderPath)) {
        searchDir = config.modelDir + "/onnx";
        textEncoderPath = searchDir + "/text_encoder.onnx";
    }
    
    std::string durationPredictorPath = searchDir + "/duration_predictor.onnx";
    std::string vectorEstimatorPath = searchDir + "/vector_estimator.onnx";
    std::string vocoderPath = searchDir + "/vocoder.onnx";
    std::string unicodeIndexerPath = searchDir + "/unicode_indexer.json";
    std::string ttsConfigPath = searchDir + "/tts.json";

    // 必须检查关键模型文件
    if (!checkFile(textEncoderPath) || !checkFile(vectorEstimatorPath) || !checkFile(vocoderPath)) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: model files not found in %{public}s or %{public}s/onnx", 
                     config.modelDir.c_str(), config.modelDir.c_str());
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: need text_encoder.onnx + vector_estimator.onnx + vocoder.onnx");
        return false;
    }
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: all model files found in %{public}s", searchDir.c_str());

    // 3. 加载 unicode tokenizer (替代 tokens.txt)
    if (checkFile(unicodeIndexerPath)) {
        if (!LoadUnicodeIndexer(unicodeIndexerPath)) {
            OH_LOG_WARN(LOG_APP, "SupertonicTTS: unicode_indexer.json load failed, using fallback");
        }
    } else {
        OH_LOG_WARN(LOG_APP, "SupertonicTTS: unicode_indexer.json not found, using unicode fallback");
    }

    // 4. 加载声音风格
    std::string stylePath = config.modelDir + "/voice_styles/" + config.voiceStyle + ".json";
    if (!checkFile(stylePath)) {
        // 也在 searchDir 下找 voice_styles
        stylePath = searchDir + "/voice_styles/" + config.voiceStyle + ".json";
    }
    if (checkFile(stylePath)) {
        LoadVoiceStyle(stylePath);
    } else {
        OH_LOG_WARN(LOG_APP, "SupertonicTTS: voice style not found: %{public}s, using default", 
                    stylePath.c_str());
        m_currentVoice.name = config.voiceStyle;
    }

    // 5. 加载 ONNX 模型 (仅演示加载入口，实际推理在 Synthesize 中)
    if (checkFile(textEncoderPath)) {
        LoadOnnxModel(textEncoderPath);
    }

    m_initialized = true;
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: initialized OK, modelDir=%{public}s, voice=%{public}s", 
                config.modelDir.c_str(), config.voiceStyle.c_str());
    return true;
}

void SupertonicTtsEngine::Release() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_initialized = false;
    m_isBusy.store(false);
    m_interrupted.store(false);

    // 释放 ONNX Runtime session (通过 ORT API)
    // Ort::Session 析构会自动释放，这里用 dlclose 管理
    m_session = nullptr;
    m_env = nullptr;

    OH_LOG_INFO(LOG_APP, "SupertonicTTS: released");
}

std::vector<std::string> SupertonicTtsEngine::GetAvailableVoices() const {
    std::vector<std::string> voices;
    
    // 从 voice_styles 目录扫描可用风格
    std::string styleDir = m_config.modelDir + "/voice_styles/";
    
    // 默认支持的风格列表
    const char* defaultVoices[] = {
        "M1", "M2", "M3", "M4", "M5",
        "F1", "F2", "F3", "F4", "F5"
    };
    
    for (auto& v : defaultVoices) {
        std::string path = styleDir + v + ".json";
        FILE* f = fopen(path.c_str(), "rb");
        if (f) {
            fclose(f);
            voices.push_back(v);
        }
    }
    
    return voices;
}

bool SupertonicTtsEngine::SetVoiceStyle(const std::string& voiceName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::string stylePath = m_config.modelDir + "/voice_styles/" + voiceName + ".json";
    FILE* f = fopen(stylePath.c_str(), "rb");
    if (!f) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: voice style not found: %{public}s", 
                     stylePath.c_str());
        return false;
    }
    fclose(f);
    
    m_currentVoice.name = voiceName;
    LoadVoiceStyle(stylePath);
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: switched to voice: %{public}s", voiceName.c_str());
    return true;
}

std::string SupertonicTtsEngine::GetCurrentVoiceStyle() const {
    return m_currentVoice.name;
}

bool SupertonicTtsEngine::Synthesize(
    const std::string& text,
    const std::string& lang,
    float speed,
    int totalSteps,
    std::vector<float>& outAudio,
    int32_t& outSampleRate)
{
    if (!m_initialized) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: not initialized");
        return false;
    }

    m_isBusy.store(true);
    m_interrupted.store(false);

    OH_LOG_INFO(LOG_APP, "SupertonicTTS: synthesize lang=%{public}s, speed=%.2f, steps=%{public}d, text='%{public}s'",
                lang.c_str(), speed, totalSteps, text.c_str());

    // ---- 推理管线 (占位：实际模型加载后填充) ----
    // Step 1: 文本分词 → token IDs
    auto tokens = Tokenize(text);
    
    // Step 2: 运行 encoder → 获取文本嵌入
    // TODO: Ort::Session::Run(...) 调用 encoder.onnx
    
    // Step 3: 运行 flow-matching decoder → 生成 mel 频谱
    // TODO: 输入文本嵌入 + 声音风格嵌入 → decoder.onnx
    
    // Step 4: 运行 vocoder → 生成波形
    // TODO: mel 频谱 → vocoder.onnx → float32 PCM
    
    // Step 5: 后处理 (增益、去噪)
    // TODO: ...

    // ---- 模拟输出 (模型未加载时生成提示音) ----
    // 实际模型加载后，此部分会被真实的推理管线替代
    outSampleRate = 44100;
    float duration = 1.0f; // 1 秒测试音
    size_t numSamples = static_cast<size_t>(outSampleRate * duration);
    outAudio.resize(numSamples);
    
    for (size_t i = 0; i < numSamples; i++) {
        // 生成一个 440Hz 正弦波作为占位
        float t = static_cast<float>(i) / outSampleRate;
        outAudio[i] = 0.3f * sinf(2.0f * 3.14159f * 440.0f * t);
        // 淡入淡出
        float fade = 1.0f;
        if (i < 1000) fade = static_cast<float>(i) / 1000.0f;
        if (i > numSamples - 1000) fade = static_cast<float>(numSamples - i) / 1000.0f;
        outAudio[i] *= fade;
    }

    OH_LOG_INFO(LOG_APP, "SupertonicTTS: synthesized %zu samples (%zu ms) [PLACEHOLDER - model needed]", 
                numSamples, static_cast<size_t>(duration * 1000));

    m_isBusy.store(false);
    return true;
}

bool SupertonicTtsEngine::SynthesizeStreaming(
    const std::string& text,
    const std::string& lang,
    float speed,
    int totalSteps,
    SupertonicAudioCallback callback)
{
    if (!m_initialized || !callback) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: not initialized or no callback");
        return false;
    }

    // 将长文本切分为句子，逐句合成
    auto sentences = SplitSentences(text);
    if (sentences.empty()) {
        sentences.push_back(text);
    }

    OH_LOG_INFO(LOG_APP, "SupertonicTTS: streaming %zu sentences", sentences.size());

    m_isBusy.store(true);
    m_interrupted.store(false);

    size_t totalSentences = sentences.size();
    for (size_t i = 0; i < totalSentences; i++) {
        if (m_interrupted.load()) {
            OH_LOG_INFO(LOG_APP, "SupertonicTTS: streaming interrupted at sentence %zu/%zu", 
                       i, totalSentences);
            break;
        }

        std::vector<float> audio;
        int32_t sampleRate = 44100;
        
        bool ok = Synthesize(sentences[i], lang, speed, totalSteps, audio, sampleRate);
        if (!ok) {
            OH_LOG_ERROR(LOG_APP, "SupertonicTTS: sentence %zu/%zu failed", i, totalSentences);
            continue;
        }

        bool isLast = (i == totalSentences - 1);
        float progress = static_cast<float>(i + 1) / totalSentences;

        // 调用回调
        callback(audio.data(), audio.size(), sampleRate, progress, isLast);

        // 句间 300ms 停顿
        if (!isLast && !m_interrupted.load()) {
            size_t pauseSamples = static_cast<size_t>(sampleRate * 0.3f);
            std::vector<float> silence(pauseSamples, 0.0f);
            callback(silence.data(), silence.size(), sampleRate, progress, false);
        }
    }

    m_isBusy.store(false);
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: streaming complete");
    return true;
}

void SupertonicTtsEngine::Interrupt() {
    m_interrupted.store(true);
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: interrupt signal sent");
}

std::string SupertonicTtsEngine::GetVersionInfo() const {
    return "SupertonicTTS 1.0.0 (compatible with Supertonic 3 ONNX models)";
}

bool SupertonicTtsEngine::CheckModelFiles(const std::string& modelDir) {
    auto exists = [](const std::string& path) -> bool {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fclose(f);
        return true;
    };

    // 支持两种目录结构：直接目录 或 onnx/ 子目录
    if (exists(modelDir + "/text_encoder.onnx") && 
        exists(modelDir + "/vector_estimator.onnx") && 
        exists(modelDir + "/vocoder.onnx")) {
        return true;
    }
    if (exists(modelDir + "/onnx/text_encoder.onnx") && 
        exists(modelDir + "/onnx/vector_estimator.onnx") && 
        exists(modelDir + "/onnx/vocoder.onnx")) {
        return true;
    }
    
    OH_LOG_ERROR(LOG_APP, "SupertonicTTS: no model files in %{public}s", modelDir.c_str());
    return false;
}

// ============================================================
// 内部实现
// ============================================================

bool SupertonicTtsEngine::LoadOnnxModel(const std::string& modelPath) {
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: loading model: %{public}s", modelPath.c_str());
    // TODO: 使用 ONNX Runtime C++ API 创建 Session
    // 示例:
    //   Ort::SessionOptions sessionOptions;
    //   sessionOptions.SetIntraOpNumThreads(m_config.numThreads);
    //   sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    //   m_session = new Ort::Session(*env, modelPath.c_str(), sessionOptions);
    return true;
}

bool SupertonicTtsEngine::LoadVoiceStyle(const std::string& stylePath) {
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: loading voice style: %{public}s", stylePath.c_str());
    
    // 解析 JSON 格式的声音风格文件
    // 格式: {"name": "M1", "embedding": [...], "speaker_id": 0}
    std::ifstream file(stylePath);
    if (!file.is_open()) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: cannot open voice style: %{public}s", 
                     stylePath.c_str());
        return false;
    }

    // 简单解析 (实际应使用 JSON 库)
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // 提取 name
    auto namePos = content.find("\"name\"");
    if (namePos != std::string::npos) {
        auto quote1 = content.find('"', namePos + 6);
        auto quote2 = content.find('"', quote1 + 1);
        if (quote1 != std::string::npos && quote2 != std::string::npos) {
            m_currentVoice.name = content.substr(quote1 + 1, quote2 - quote1 - 1);
        }
    }
    
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: voice style loaded: %{public}s", 
                m_currentVoice.name.c_str());
    return true;
}

bool SupertonicTtsEngine::LoadUnicodeIndexer(const std::string& indexPath) {
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: loading unicode indexer: %{public}s", indexPath.c_str());
    
    std::ifstream file(indexPath);
    if (!file.is_open()) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: cannot open unicode indexer: %{public}s", indexPath.c_str());
        return false;
    }
    
    // 解析 JSON 数组: [id0, id1, id2, ...]
    // 数组索引 = unicode 码点, 值 = tokenID (-1 表示不支持)
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // 简单解析: 跳过 [ 和 ]，按逗号分割
    m_unicodeIndexer.clear();
    
    size_t start = content.find('[');
    size_t end = content.find(']');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        OH_LOG_ERROR(LOG_APP, "SupertonicTTS: invalid unicode indexer format");
        return false;
    }
    
    std::string numbers = content.substr(start + 1, end - start - 1);
    size_t pos = 0;
    while (pos < numbers.size()) {
        // 跳过空白
        while (pos < numbers.size() && numbers[pos] == ' ') pos++;
        if (pos >= numbers.size()) break;
        
        // 读取数字
        size_t next = numbers.find(',', pos);
        if (next == std::string::npos) next = numbers.size();
        
        std::string token = numbers.substr(pos, next - pos);
        // 去除空白
        while (!token.empty() && token.back() == ' ') token.pop_back();
        
        if (!token.empty() && token != "-1") {
            m_unicodeIndexer.push_back(std::stoi(token));
        } else if (token == "-1") {
            m_unicodeIndexer.push_back(-1);
        }
        
        pos = next + 1;
    }
    
    OH_LOG_INFO(LOG_APP, "SupertonicTTS: loaded unicode indexer with %{public}zu entries", 
                m_unicodeIndexer.size());
    return true;
}

std::vector<int64_t> SupertonicTtsEngine::Tokenize(const std::string& text) {
    // Supertonic 3 使用 unicode 字符级 tokenizer
    // unicode_indexer: index=unicode码点, value=tokenID (-1=不支持)
    std::vector<int64_t> tokens;
    tokens.reserve(text.size() + 2);
    
    // Supertonic 模型特殊 token ID (根据 tts.json 推断):
    // 0 = PAD, 1 = BOS, 2 = EOS
    constexpr int64_t BOS_TOKEN = 1;
    constexpr int64_t EOS_TOKEN = 2;
    
    // BOS token
    tokens.push_back(BOS_TOKEN);
    
    // 逐字符查找 unicode indexer 中的 token ID
    for (size_t i = 0; i < text.size(); ) {
        // 获取 unicode 码点 (UTF-8 解码)
        uint32_t codePoint = 0;
        unsigned char c = static_cast<unsigned char>(text[i]);
        
        if (c < 0x80) {
            // 1-byte: 0xxxxxxx
            codePoint = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte: 110xxxxx 10xxxxxx
            codePoint = (c & 0x1F) << 6;
            if (i + 1 < text.size()) codePoint |= (text[i + 1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte: 1110xxxx 10xxxxxx 10xxxxxx
            codePoint = (c & 0x0F) << 12;
            if (i + 2 < text.size()) {
                codePoint |= ((text[i + 1] & 0x3F) << 6);
                codePoint |= (text[i + 2] & 0x3F);
            }
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
            codePoint = (c & 0x07) << 18;
            if (i + 3 < text.size()) {
                codePoint |= ((text[i + 1] & 0x3F) << 12);
                codePoint |= ((text[i + 2] & 0x3F) << 6);
                codePoint |= (text[i + 3] & 0x3F);
            }
            i += 4;
        } else {
            i++;
            continue;
        }
        
        // 在 unicode indexer 中查找 token ID
        int64_t tokenId = -1;
        if (codePoint < m_unicodeIndexer.size()) {
            tokenId = m_unicodeIndexer[codePoint];
        } else {
            // 对于超出 indexer 范围的 BMP 字符，直接使用码点
            // 这是 fallback 方案
            tokenId = static_cast<int64_t>(codePoint);
        }
        
        if (tokenId >= 0) {
            tokens.push_back(tokenId);
        }
    }
    
    // EOS token
    tokens.push_back(EOS_TOKEN);
    
    return tokens;
}

std::vector<std::string> SupertonicTtsEngine::SplitSentences(const std::string& text) {
    std::vector<std::string> sentences;
    
    // 按标点符号切分句子
    const char* delimiters = "。！？；.!?;\n";
    size_t start = 0;
    
    for (size_t i = 0; i <= text.size(); i++) {
        bool isDelim = false;
        for (const char* d = delimiters; *d; d++) {
            if (i < text.size() && text[i] == *d) {
                isDelim = true;
                break;
            }
        }
        
        if (isDelim || i == text.size()) {
            std::string sentence = text.substr(start, i - start + 1);
            // 去除首尾空白
            auto trimLeft = sentence.find_first_not_of(" \t\n\r");
            auto trimRight = sentence.find_last_not_of(" \t\n\r");
            if (trimLeft != std::string::npos && trimRight != std::string::npos) {
                sentence = sentence.substr(trimLeft, trimRight - trimLeft + 1);
            }
            
            if (!sentence.empty()) {
                sentences.push_back(sentence);
            }
            start = i + 1;
        }
    }
    
    // 如果没切出句子，整段作为一个
    if (sentences.empty() && !text.empty()) {
        sentences.push_back(text);
    }
    
    return sentences;
}

void SupertonicTtsEngine::FloatToInt16(const float* input, size_t n, int16_t* output) {
    for (size_t i = 0; i < n; i++) {
        float s = input[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        output[i] = static_cast<int16_t>(s * 32767.0f);
    }
}

void SupertonicTtsEngine::Int16ToFloat(const int16_t* input, size_t n, float* output) {
    for (size_t i = 0; i < n; i++) {
        output[i] = static_cast<float>(input[i]) / 32768.0f;
    }
}
