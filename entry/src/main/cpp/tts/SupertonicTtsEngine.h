#ifndef SUPERTONIC_TTS_ENGINE_H
#define SUPERTONIC_TTS_ENGINE_H

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>

/**
 * @brief Supertonic TTS 引擎
 * 
 * 加载 Supertonic 3 ONNX 模型，实现高保真语音合成。
 * 支持 31 种语言、多角色声音风格、流式输出。
 * 
 * 模型来源: https://huggingface.co/Supertone/supertonic-3
 * 模型文件 (放置于 rawfile/models/supertonic/onnx/):
 *   - text_encoder.onnx         # 文本编码器 (34MB)
 *   - duration_predictor.onnx   # 时长预测器 (3.5MB)
 *   - vector_estimator.onnx     # Flow-matching 解码器 (244MB)
 *   - vocoder.onnx              # 声码器 (96MB)
 *   - tts.json                  # 模型架构配置
 *   - unicode_indexer.json      # Unicode 字符级 tokenizer (替代 tokens.txt)
 * Voice Style 文件 (放置于 rawfile/models/supertonic/voice_styles/):
 *   - M1.json ~ M5.json    # 男声 1-5
 *   - F1.json ~ F5.json    # 女声 1-5
 * 
 * 注意: Supertonic 3 使用 unicode 字符级 tokenizer，没有 tokens.txt。
 * unicode_indexer.json 是一个数组: index=unicode码点, value=tokenID (-1=不支持)
 */

// 前向声明 ONNX Runtime 类型 (避免引入 onnxruntime.h 头文件)
// 实际使用时需链接 libonnxruntime.so

struct SupertonicTtsConfig {
    std::string modelDir;           // 模型目录路径 (沙箱路径)
    std::string voiceStyle;         // 声音风格文件名 (如 "M1", "F3")
    int numThreads = 2;             // 推理线程数
    float speed = 1.05f;            // 语速 (0.7~2.0)
    int totalSteps = 8;             // 去噪步数 (5=快, 8=中, 12=高质)
    bool debug = false;             // 调试日志
};

struct SupertonicVoiceStyle {
    std::string name;               // 名称 (M1/F1 等)
    std::vector<float> embedding;   // 声音嵌入向量
    int speakerId = 0;              // 说话人 ID
};

// TTS 合成结果回调
using SupertonicAudioCallback = std::function<void(
    const float* audio,             // float32 PCM 数据
    size_t numSamples,              // 采样数
    int sampleRate,                 // 采样率 (44100)
    float progress,                 // 进度 0.0~1.0
    bool isFinal                    // 是否最后一块
)>;

class SupertonicTtsEngine {
public:
    SupertonicTtsEngine();
    ~SupertonicTtsEngine();

    // 禁止拷贝
    SupertonicTtsEngine(const SupertonicTtsEngine&) = delete;
    SupertonicTtsEngine& operator=(const SupertonicTtsEngine&) = delete;

    // ========== 生命周期 ==========
    
    /// 初始化引擎，加载模型
    bool Init(const SupertonicTtsConfig& config);

    /// 检查引擎是否已初始化
    bool IsInitialized() const { return m_initialized; }

    /// 释放所有资源
    void Release();

    // ========== 声音风格管理 ==========

    /// 获取可用声音风格列表 ("M1", "F1" 等)
    std::vector<std::string> GetAvailableVoices() const;

    /// 切换声音风格
    bool SetVoiceStyle(const std::string& voiceName);

    /// 获取当前声音风格
    std::string GetCurrentVoiceStyle() const;

    // ========== 语音合成 ==========

    /**
     * @brief 合成语音 (完整文本，非流式)
     * @param text 输入文本
     * @param lang 语言代码 ("en", "zh", "ja" 等，"" 表示自动检测)
     * @param speed 语速 (0.7~2.0, 默认 1.05)
     * @param totalSteps 去噪步数 (5~12, 默认 8)
     * @param outAudio 输出 float32 PCM 数据
     * @param outSampleRate 输出采样率
     * @return true 成功
     */
    bool Synthesize(
        const std::string& text,
        const std::string& lang,
        float speed,
        int totalSteps,
        std::vector<float>& outAudio,
        int32_t& outSampleRate
    );

    /**
     * @brief 合成语音 (流式，逐句输出)
     * @param text 输入文本
     * @param lang 语言代码
     * @param speed 语速
     * @param totalSteps 去噪步数
     * @param callback 音频回调 (可能多次调用)
     * @return true 成功
     */
    bool SynthesizeStreaming(
        const std::string& text,
        const std::string& lang,
        float speed,
        int totalSteps,
        SupertonicAudioCallback callback
    );

    // ========== 控制 ==========

    /// 中断当前合成
    void Interrupt();

    /// 是否正在合成
    bool IsBusy() const { return m_isBusy.load(); }

    // ========== 工具 ==========

    /// 获取引擎版本信息
    std::string GetVersionInfo() const;

    /// 检查模型文件是否存在
    static bool CheckModelFiles(const std::string& modelDir);

private:
    // ========== 内部状态 ==========
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_isBusy{false};
    std::atomic<bool> m_interrupted{false};
    mutable std::mutex m_mutex;

    SupertonicTtsConfig m_config;
    SupertonicVoiceStyle m_currentVoice;

    // ========== ONNX Runtime 推理 ==========
    
    // 不透明指针 (Ort::Session 等)
    // 实际使用时 include <onnxruntime/core/session/onnxruntime_cxx_api.h>
    void* m_env = nullptr;          // Ort::Env*
    void* m_session = nullptr;      // Ort::Session*
    void* m_memoryInfo = nullptr;   // Ort::MemoryInfo*

    // ========== 模型加载 ==========

    bool LoadOnnxModel(const std::string& modelPath);
    bool LoadVoiceStyle(const std::string& stylePath);
    bool LoadUnicodeIndexer(const std::string& indexPath);

    // ========== 文本处理 ==========

    /// unicode_indexer: index=unicode码点, value=tokenID (-1=不支持)
    std::vector<int> m_unicodeIndexer;

    /// 将文本转为 token IDs (unicode 字符级)
    std::vector<int64_t> Tokenize(const std::string& text);

    /// 将长文本切分为句子
    std::vector<std::string> SplitSentences(const std::string& text);

    // ========== 音频处理 ==========

    /// float32 PCM 转 int16 PCM
    static void FloatToInt16(const float* input, size_t n, int16_t* output);

    /// int16 PCM 转 float32 PCM
    static void Int16ToFloat(const int16_t* input, size_t n, float* output);
};

#endif // SUPERTONIC_TTS_ENGINE_H
