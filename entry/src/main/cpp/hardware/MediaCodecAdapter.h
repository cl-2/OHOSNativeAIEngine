#ifndef MEDIA_CODEC_ADAPTER_H
#define MEDIA_CODEC_ADAPTER_H

#include "IHardwareCodec.h"
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <hilog/log.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

/**
 * @brief 输入缓冲区块
 */
struct InputPacket {
    std::vector<uint8_t> data;
    int64_t pts;
};

/**
 * @brief 输出缓冲区块（解码后的帧）
 */
struct OutputFrame {
    std::vector<uint8_t> data;
    int64_t pts;
    uint32_t flags;
    int32_t width;
    int32_t height;
};

/**
 * @brief 鸿蒙平台 MediaCodec 适配器
 * 
 * 使用 HarmonyOS Native AVCodec API 的异步回调模式，
 * 内部维护输入/输出缓冲区队列，通过条件变量同步。
 */
class MediaCodecAdapter : public IHardwareCodec {
public:
    MediaCodecAdapter();
    virtual ~MediaCodecAdapter();

    bool Init(const std::string& mimeType, bool isEncoder) override;
    bool QueueInput(const uint8_t* data, size_t size, int64_t pts) override;
    bool DequeueOutput(std::vector<uint8_t>& outData, int64_t& outPts) override;
    void Flush() override;
    void Release() override;
    std::string GetName() const override { return "OHOS_MediaCodec"; }

    // 辅助方法
    bool IsRunning() const { return m_isRunning.load(); }
    size_t InputQueueSize() const { std::lock_guard<std::mutex> lk(m_inputMutex); return m_inputQueue.size(); }
    size_t OutputQueueSize() const { std::lock_guard<std::mutex> lk(m_outputMutex); return m_outputQueue.size(); }

private:
    OH_AVCodec* m_codec = nullptr;
    bool m_isInitialized = false;
    std::atomic<bool> m_isRunning{false};

    // ---- 输入缓冲区队列（生产者：用户线程推送数据）----
    mutable std::mutex m_inputMutex;
    std::condition_variable m_inputCv;
    std::queue<InputPacket> m_inputQueue;
    bool m_inputEosSent = false;         // 是否已发送结束标志

    // ---- 输出缓冲区队列（消费者：用户线程拉取解码结果）----
    mutable std::mutex m_outputMutex;
    std::condition_variable m_outputCv;
    std::queue<OutputFrame> m_outputQueue;
    bool m_outputEosReceived = false;    // 是否已收到结束标志

    // 媒体格式描述
    std::string m_mimeType;
    int32_t m_width = 0;
    int32_t m_height = 0;

    // ---- 回调函数 (HarmonyOS Native AVCodec 静态回调) ----
    static void OnError(OH_AVCodec* codec, int32_t errorCode, void* userData);
    static void OnStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData);
    static void OnNeedInputData(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* data, void* userData);
    static void OnNeedOutputData(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* data, void* userData);
};

#endif // MEDIA_CODEC_ADAPTER_H
