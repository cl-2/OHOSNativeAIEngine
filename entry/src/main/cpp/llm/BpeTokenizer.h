#ifndef BPE_TOKENIZER_H
#define BPE_TOKENIZER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>

/**
 * @brief BPE 分词器 — 兼容 HuggingFace tokenizer.json 格式
 *
 * 专门为 Qwen2.5 系列优化，支持：
 *   - Byte-level BPE（基于 tokenizer.json 的 vocab + merges）
 *   - 特殊 token 处理（<|im_start|>, <|im_end|> 等）
 *   - 对话模板构建（ChatML 格式）
 *   - 流式安全解码（字节级增量解码）
 *
 * 使用方式：
 *   BpeTokenizer tokenizer;
 *   if (!tokenizer.Load("path/to/tokenizer.json")) { ... }
 *   auto ids = tokenizer.Encode("你好");
 *   auto text = tokenizer.Decode(ids);
 *
 * 工程原则：
 *   - 无第三方依赖（纯 C++17 + 标准库）
 *   - 可独立测试
 *   - 线程安全（每实例独立状态）
 */

class BpeTokenizer {
public:
    BpeTokenizer();
    ~BpeTokenizer();

    BpeTokenizer(const BpeTokenizer&) = delete;
    BpeTokenizer& operator=(const BpeTokenizer&) = delete;

    // ========== 生命周期 ==========

    /// 从 tokenizer.json 字符串加载（文件内容）
    bool LoadFromJson(const std::string& jsonContent);

    /// 从 tokenizer.json 文件路径加载
    bool Load(const std::string& filePath);

    /// 是否已成功加载
    bool IsLoaded() const { return m_loaded; }

    // ========== 编码 ==========

    /// 将文本编码为 token ID 序列
    std::vector<int32_t> Encode(const std::string& text) const;

    /// 批量编码：将 messages [{role, content}] 编码为模型输入
    /// 返回拼接后的 token ID 序列（含 ChatML 模板）
    std::vector<int32_t> EncodeMessages(
        const std::vector<std::pair<std::string, std::string>>& messages,
        bool addGenerationPrompt = true) const;

    // ========== 解码 ==========

    /// 将 token ID 序列解码为文本
    std::string Decode(const std::vector<int32_t>& ids) const;

    /// 安全解码单个 token（用于流式输出，处理字节级不完整 UTF-8）
    std::string DecodeToken(int32_t id) const;

    // ========== 查询 ==========

    int32_t VocabSize() const { return m_vocabSize; }
    int32_t PadTokenId() const { return m_padTokenId; }

    /// Qwen2.5 特殊 token
    int32_t ImStartId() const { return 151644; }
    int32_t ImEndId() const { return 151645; }
    int32_t EndOfTextId() const { return 151643; }

private:
    // ========== 内部结构 ==========

    /// BPE merge 优先级
    using MergePair = std::pair<std::string, std::string>;
    struct MergeRank {
        int32_t rank;
    };

    // ========== 内部方法 ==========

    /// 预分词：将文本拆分为初始字节 token
    std::vector<std::string> PreTokenize(const std::string& text) const;

    /// 对单个词应用 BPE merge
    std::vector<std::string> ApplyBpe(const std::string& word) const;

    /// 查找 token ID（若不存在返回 -1）
    int32_t TokenToId(const std::string& token) const;

    /// 字节级编码：将字符映射到字节
    static std::vector<uint8_t> Utf8ToBytes(const std::string& text);
    static std::string BytesToUtf8(const std::vector<uint8_t>& bytes);

    /// 构建字节到 unicode 的映射表（GPT-2 风格 byte-level BPE）
    static std::unordered_map<uint8_t, std::string> BuildByteEncoder();
    static std::unordered_map<std::string, uint8_t> BuildByteDecoder();

    /// 将字符串拆分为 Unicode 码点
    static std::vector<uint32_t> Utf8ToCodepoints(const std::string& s);
    static std::string CodepointsToUtf8(const std::vector<uint32_t>& codepoints);

    // ========== 数据 ==========

    bool m_loaded = false;
    int32_t m_vocabSize = 0;

    // vocab: token字符串 → ID
    std::unordered_map<std::string, int32_t> m_tokenToId;
    // id → token字符串
    std::unordered_map<int32_t, std::string> m_idToToken;

    // merges: 合并优先级 (pair_string → rank)
    std::unordered_map<std::string, int32_t> m_mergeRanks;  // key = "a b" (space separated)

    // 特殊 token
    std::unordered_map<std::string, int32_t> m_specialTokens;
    int32_t m_padTokenId = 0;

    // 字节编码器（GPT-2 风格，byte→unicode char 映射）
    std::unordered_map<uint8_t, std::string> m_byteEncoder;
    std::unordered_map<std::string, uint8_t> m_byteDecoder;
};

#endif // BPE_TOKENIZER_H
