#include "BpeTokenizer.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cmath>

// ============================================================
// 最小化 JSON 解析器（专用于 tokenizer.json 结构，非通用）
// ============================================================

namespace {

/// 跳过空白
static void SkipWhitespace(const char*& p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
}

/// 解析字符串字面量 "..."（处理转义）
static std::string ParseJsonString(const char*& p) {
    SkipWhitespace(p);
    if (*p != '"') return "";
    p++; // skip "
    std::string result;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case 'u': {
                    // Unicode escape \uXXXX — simplified
                    char hex[5] = {p[1], p[2], p[3], p[4], 0};
                    unsigned int cp = strtoul(hex, nullptr, 16);
                    p += 4;
                    if (cp < 0x80) result += (char)cp;
                    else if (cp < 0x800) { result += (char)(0xC0 | (cp >> 6)); result += (char)(0x80 | (cp & 0x3F)); }
                    else { result += (char)(0xE0 | (cp >> 12)); result += (char)(0x80 | ((cp >> 6) & 0x3F)); result += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: result += *p; break;
            }
        } else {
            result += *p;
        }
        p++;
    }
    if (*p == '"') p++; // skip closing "
    return result;
}

/// 解析整数
static int64_t ParseJsonInt(const char*& p) {
    SkipWhitespace(p);
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    int64_t val = 0;
    while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
    return neg ? -val : val;
}

/// 解析浮点数
static double ParseJsonDouble(const char*& p) {
    SkipWhitespace(p);
    char* end = nullptr;
    double val = strtod(p, &end);
    if (end > p) p = end;
    return val;
}

/// 解析 JSON 值（简化：返回 string 的 map/array 形式）
/// vocab 解析：{"token": id, ...} → 填充 token→id 映射
static void ParseJsonVocab(const char*& p,
    std::unordered_map<std::string, int32_t>& tokenToId,
    std::unordered_map<int32_t, std::string>& idToToken) {
    SkipWhitespace(p);
    if (*p != '{') return;
    p++; // skip {
    while (*p && *p != '}') {
        SkipWhitespace(p);
        std::string token = ParseJsonString(p);
        SkipWhitespace(p);
        if (*p == ':') p++;
        SkipWhitespace(p);
        int32_t id = (int32_t)ParseJsonInt(p);
        tokenToId[token] = id;
        idToToken[id] = token;
        SkipWhitespace(p);
        if (*p == ',') p++;
    }
    if (*p == '}') p++;
}

/// 解析 JSON 数组 ["a b", "c d", ...] → merges
static void ParseJsonStringArray(const char*& p, std::vector<std::string>& out) {
    SkipWhitespace(p);
    if (*p != '[') return;
    p++; // skip [
    while (*p && *p != ']') {
        SkipWhitespace(p);
        std::string s = ParseJsonString(p);
        out.push_back(s);
        SkipWhitespace(p);
        if (*p == ',') p++;
    }
    if (*p == ']') p++;
}

/// 跳过一个 JSON 值（不解析，仅用来跳过无用字段）
static void SkipJsonValue(const char*& p) {
    SkipWhitespace(p);
    if (!*p) return;
    if (*p == '"') { ParseJsonString(p); return; }
    if (*p == '{') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') { const char* tmp = p; ParseJsonString(tmp); p = tmp; continue; }
            p++;
        }
        if (*p == '}') p++;
        return;
    }
    if (*p == '[') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') { const char* tmp = p; ParseJsonString(tmp); p = tmp; continue; }
            p++;
        }
        if (*p == ']') p++;
        return;
    }
    // number / true / false / null
    while (*p && *p != ',' && *p != '}' && *p != ']' && !(*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')) p++;
}

} // anonymous namespace

// ============================================================
// Byte-level BPE 编码器（GPT-2 风格）
// ============================================================

BpeTokenizer::BpeTokenizer()
    : m_byteEncoder(BuildByteEncoder()),
      m_byteDecoder(BuildByteDecoder()) {}

BpeTokenizer::~BpeTokenizer() = default;

std::unordered_map<uint8_t, std::string> BpeTokenizer::BuildByteEncoder() {
    std::unordered_map<uint8_t, std::string> encoder;
    // GPT-2 style byte-to-unicode mapping
    // Map bytes 0-255 to unicode chars starting from ! (0x21)
    int n = 0;
    for (int b = 0; b < 256; b++) {
        if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255)) {
            encoder[(uint8_t)b] = std::string(1, (char)b);
        } else {
            encoder[(uint8_t)b] = std::string(1, (char)(256 + n));
            n++;
        }
    }
    return encoder;
}

std::unordered_map<std::string, uint8_t> BpeTokenizer::BuildByteDecoder() {
    auto enc = BuildByteEncoder();
    std::unordered_map<std::string, uint8_t> decoder;
    for (auto& [b, s] : enc) {
        decoder[s] = b;
    }
    return decoder;
}

// ============================================================
// UTF-8 工具
// ============================================================

std::vector<uint32_t> BpeTokenizer::Utf8ToCodepoints(const std::string& s) {
    std::vector<uint32_t> result;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            result.push_back(c);
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            uint32_t cp = c & 0x1F;
            if (i + 1 < s.size()) { cp = (cp << 6) | ((unsigned char)s[i+1] & 0x3F); i += 2; }
            else { result.push_back(0xFFFD); i++; }
            result.push_back(cp);
        } else if ((c & 0xF0) == 0xE0) {
            uint32_t cp = c & 0x0F;
            if (i + 2 < s.size()) { cp = (cp << 6) | ((unsigned char)s[i+1] & 0x3F); cp = (cp << 6) | ((unsigned char)s[i+2] & 0x3F); i += 3; }
            else { result.push_back(0xFFFD); i++; }
            result.push_back(cp);
        } else if ((c & 0xF8) == 0xF0) {
            uint32_t cp = c & 0x07;
            if (i + 3 < s.size()) { cp = (cp << 6) | ((unsigned char)s[i+1] & 0x3F); cp = (cp << 6) | ((unsigned char)s[i+2] & 0x3F); cp = (cp << 6) | ((unsigned char)s[i+3] & 0x3F); i += 4; }
            else { result.push_back(0xFFFD); i++; }
            result.push_back(cp);
        } else {
            result.push_back(0xFFFD);
            i++;
        }
    }
    return result;
}

std::string BpeTokenizer::CodepointsToUtf8(const std::vector<uint32_t>& codepoints) {
    std::string result;
    for (uint32_t cp : codepoints) {
        if (cp < 0x80) result += (char)cp;
        else if (cp < 0x800) { result += (char)(0xC0 | (cp >> 6)); result += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { result += (char)(0xE0 | (cp >> 12)); result += (char)(0x80 | ((cp >> 6) & 0x3F)); result += (char)(0x80 | (cp & 0x3F)); }
        else { result += (char)(0xF0 | (cp >> 18)); result += (char)(0x80 | ((cp >> 12) & 0x3F)); result += (char)(0x80 | ((cp >> 6) & 0x3F)); result += (char)(0x80 | (cp & 0x3F)); }
    }
    return result;
}

std::vector<uint8_t> BpeTokenizer::Utf8ToBytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

std::string BpeTokenizer::BytesToUtf8(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

// ============================================================
// 加载 tokenizer.json
// ============================================================

bool BpeTokenizer::Load(const std::string& filePath) {
    std::ifstream in(filePath, std::ios::binary);
    if (!in.is_open()) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    return LoadFromJson(ss.str());
}

bool BpeTokenizer::LoadFromJson(const std::string& jsonContent) {
    const char* p = jsonContent.c_str();
    SkipWhitespace(p);
    if (*p != '{') return false;
    p++; // skip {

    while (*p && *p != '}') {
        SkipWhitespace(p);
        std::string key = ParseJsonString(p);
        SkipWhitespace(p);
        if (*p == ':') p++;
        SkipWhitespace(p);

        if (key == "model") {
            // 进入 model 对象
            SkipWhitespace(p);
            if (*p == '{') {
                p++; // skip {
                while (*p && *p != '}') {
                    SkipWhitespace(p);
                    std::string mKey = ParseJsonString(p);
                    SkipWhitespace(p);
                    if (*p == ':') p++;
                    SkipWhitespace(p);

                    if (mKey == "vocab") {
                        ParseJsonVocab(p, m_tokenToId, m_idToToken);
                    } else if (mKey == "merges") {
                        std::vector<std::string> merges;
                        ParseJsonStringArray(p, merges);
                        for (size_t i = 0; i < merges.size(); i++) {
                            m_mergeRanks[merges[i]] = (int32_t)i;
                        }
                    } else {
                        SkipJsonValue(p);
                    }
                    SkipWhitespace(p);
                    if (*p == ',') p++;
                }
                if (*p == '}') p++;
            }
        } else if (key == "added_tokens") {
            // 跳过 added_tokens 数组（已含在 vocab 中）
            SkipJsonValue(p);
        } else {
            SkipJsonValue(p);
        }
        SkipWhitespace(p);
        if (*p == ',') p++;
    }

    m_vocabSize = (int32_t)m_tokenToId.size();

    // 设置特殊 token
    m_specialTokens["<|im_start|>"] = 151644;
    m_specialTokens["<|im_end|>"] = 151645;
    m_specialTokens["<|im_sep|>"] = 151646;
    m_specialTokens["<|endoftext|>"] = 151643;
    m_padTokenId = 151643;

    m_loaded = m_vocabSize > 0;
    return m_loaded;
}

// ============================================================
// 预分词
// ============================================================

std::vector<std::string> BpeTokenizer::PreTokenize(const std::string& text) const {
    // GPT-2 style pre-tokenization: split on whitespace and punctuation
    // Regex pattern: /'s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+/g
    // Simplified: split by spaces and non-alphanumeric boundaries

    std::vector<std::string> words;
    std::string current;
    enum class Mode { None, Letter, Number, Other, Space };

    Mode mode = Mode::None;
    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = (unsigned char)text[i];
        // Get codepoint length
        int len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;

        std::string ch = text.substr(i, len);
        bool isSpace = (len == 1 && (c == ' ' || c == '\t' || c == '\n' || c == '\r'));
        bool isLetter = !isSpace && (len > 1 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 128);
        bool isNumber = !isSpace && !isLetter && (len == 1 && c >= '0' && c <= '9');

        Mode newMode = isSpace ? Mode::Space : (isLetter ? Mode::Letter : (isNumber ? Mode::Number : Mode::Other));

        if (newMode != mode && !current.empty()) {
            if (!(mode == Mode::Space && newMode != Mode::Space)) {
                words.push_back(current);
                current.clear();
            }
        }

        if (isSpace) {
            // GPT-2: prepend space to next non-space word
            if (current.empty() && newMode == Mode::Space) {
                // Skip leading spaces — they'll be part of the next word
            }
            current += ch;
        } else {
            if (mode == Mode::Space && !current.empty()) {
                // Keep space prefix with the word
                words.push_back(current);
                current.clear();
            }
            current += ch;
        }

        mode = newMode;
        i += len;
    }
    if (!current.empty()) words.push_back(current);

    // Convert words to byte-encoded form
    std::vector<std::string> encoded;
    for (auto& w : words) {
        std::string encodedWord;
        for (unsigned char b : w) {
            auto it = m_byteEncoder.find(b);
            if (it != m_byteEncoder.end()) {
                encodedWord += it->second;
            } else {
                encodedWord += (char)b;
            }
        }
        encoded.push_back(encodedWord);
    }

    return encoded;
}

// ============================================================
// BPE Merge
// ============================================================

std::vector<std::string> BpeTokenizer::ApplyBpe(const std::string& word) const {
    // Start with individual characters
    std::vector<std::string> symbols;
    for (size_t i = 0; i < word.size(); ) {
        unsigned char c = (unsigned char)word[i];
        int len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        symbols.push_back(word.substr(i, len));
        i += len;
    }

    if (symbols.size() <= 1) return symbols;

    // Iteratively merge the best pair
    while (symbols.size() > 1) {
        // Find the best pair (lowest rank)
        int32_t bestRank = INT32_MAX;
        size_t bestIdx = 0;

        for (size_t i = 0; i + 1 < symbols.size(); i++) {
            std::string pair = symbols[i] + " " + symbols[i + 1];
            auto it = m_mergeRanks.find(pair);
            if (it != m_mergeRanks.end() && it->second < bestRank) {
                bestRank = it->second;
                bestIdx = i;
            }
        }

        if (bestRank == INT32_MAX) break; // No more merges possible

        // Merge the pair
        std::string merged = symbols[bestIdx] + symbols[bestIdx + 1];
        std::vector<std::string> newSymbols;
        for (size_t i = 0; i < symbols.size(); i++) {
            if (i == bestIdx) {
                newSymbols.push_back(merged);
                i++; // skip the next one
            } else {
                newSymbols.push_back(symbols[i]);
            }
        }
        symbols.swap(newSymbols);
    }

    return symbols;
}

// ============================================================
// ID 查找
// ============================================================

int32_t BpeTokenizer::TokenToId(const std::string& token) const {
    auto it = m_tokenToId.find(token);
    if (it != m_tokenToId.end()) return it->second;

    // Check special tokens
    auto sit = m_specialTokens.find(token);
    if (sit != m_specialTokens.end()) return sit->second;

    return -1;
}

// ============================================================
// 编码
// ============================================================

std::vector<int32_t> BpeTokenizer::Encode(const std::string& text) const {
    std::vector<int32_t> ids;

    // Check for special tokens in the text
    // Qwen2.5 special tokens start with <|
    std::string remaining = text;
    while (!remaining.empty()) {
        // Look for <|...|> patterns
        bool foundSpecial = false;
        for (auto& [token, id] : m_specialTokens) {
            if (remaining.substr(0, token.size()) == token) {
                ids.push_back(id);
                remaining = remaining.substr(token.size());
                foundSpecial = true;
                break;
            }
        }
        if (foundSpecial) continue;

        // Normal BPE encoding
        break;
    }

    if (remaining.empty()) return ids;

    // Pre-tokenize
    auto words = PreTokenize(remaining);

    // Apply BPE to each word
    for (auto& word : words) {
        // First check if word exists directly in vocab
        int32_t directId = TokenToId(word);
        if (directId >= 0) {
            ids.push_back(directId);
            continue;
        }

        // Apply BPE merges
        auto bpeTokens = ApplyBpe(word);
        for (auto& token : bpeTokens) {
            int32_t id = TokenToId(token);
            if (id >= 0) {
                ids.push_back(id);
            } else {
                // Unknown token — use fallback byte encoding
                for (unsigned char b : word) {
                    std::string byteStr = m_byteEncoder.at(b);
                    int32_t byteId = TokenToId(byteStr);
                    if (byteId >= 0) ids.push_back(byteId);
                }
                break;
            }
        }
    }

    return ids;
}

std::vector<int32_t> BpeTokenizer::EncodeMessages(
    const std::vector<std::pair<std::string, std::string>>& messages,
    bool addGenerationPrompt) const
{
    std::vector<int32_t> ids;

    for (auto& msg : messages) {
        // <|im_start|>role\ncontent<|im_end|>\n
        ids.push_back(ImStartId());
        auto roleIds = Encode(msg.first);
        ids.insert(ids.end(), roleIds.begin(), roleIds.end());
        ids.push_back('\n'); // newline after role
        auto contentIds = Encode(msg.second);
        ids.insert(ids.end(), contentIds.begin(), contentIds.end());
        ids.push_back(ImEndId());
        ids.push_back('\n');
    }

    if (addGenerationPrompt) {
        // <|im_start|>assistant\n
        ids.push_back(ImStartId());
        auto roleIds = Encode("assistant");
        ids.insert(ids.end(), roleIds.begin(), roleIds.end());
        ids.push_back('\n');
    }

    return ids;
}

// ============================================================
// 解码
// ============================================================

std::string BpeTokenizer::DecodeToken(int32_t id) const {
    auto it = m_idToToken.find(id);
    if (it == m_idToToken.end()) return "";

    // Skip special tokens
    if (id == ImStartId() || id == ImEndId() || id == EndOfTextId()) return "";

    // Decode byte-level tokens
    std::string result;
    for (char c : it->second) {
        auto dit = m_byteDecoder.find(std::string(1, c));
        if (dit != m_byteDecoder.end()) {
            result += (char)dit->second;
        } else {
            result += c;
        }
    }
    return result;
}

std::string BpeTokenizer::Decode(const std::vector<int32_t>& ids) const {
    std::string result;
    for (int32_t id : ids) {
        result += DecodeToken(id);
    }
    return result;
}
