#include "MediaCodecAdapter.h"
#include <cstring>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_TAG "MediaCodecHAL"
#define LOG_DOMAIN 0x0100

// ============================================================
// MediaCodecAdapter - 鸿蒙平台硬件编解码适配器
// 使用 HarmonyOS Native AVCodec 异步回调 API
// 内部维护双队列（输入/输出），通过条件变量同步
// ============================================================

MediaCodecAdapter::MediaCodecAdapter()
    : m_codec(nullptr)
    , m_isInitialized(false)
    , m_isRunning(false)
    , m_inputEosSent(false)
    , m_outputEosReceived(false)
{
}

MediaCodecAdapter::~MediaCodecAdapter() {
    Release();
}

bool MediaCodecAdapter::Init(const std::string& mimeType, bool isEncoder) {
    if (m_isInitialized) return true;

    m_mimeType = mimeType;

    // 1. 创建编解码器
    if (isEncoder) {
        // 编码器暂不实现，仅解码器示例
        OH_LOG_WARN(LOG_APP, "MediaCodecAdapter: encoder not implemented yet, use decoder path");
        return false;
    }

    m_codec = OH_VideoDecoder_CreateByMime(mimeType.c_str());
    if (!m_codec) {
        OH_LOG_ERROR(LOG_APP, "MediaCodecAdapter: Failed to create decoder for mime: %{public}s", mimeType.c_str());
        return false;
    }
    OH_LOG_INFO(LOG_APP, "MediaCodecAdapter: created decoder for %{public}s", mimeType.c_str());

    // 2. 注册异步回调
    OH_AVCodecCallback callback;
    callback.onError = OnError;
    callback.onStreamChanged = OnStreamChanged;
    callback.onNeedInputBuffer = OnNeedInputData;
    callback.onNewOutputBuffer = OnNeedOutputData;

    if (OH_VideoDecoder_RegisterCallback(m_codec, callback, this) != AV_ERR_OK) {
        OH_LOG_ERROR(LOG_APP, "MediaCodecAdapter: Failed to register callback");
        OH_VideoDecoder_Destroy(m_codec);
        m_codec = nullptr;
        return false;
    }
    OH_LOG_INFO(LOG_APP, "MediaCodecAdapter: callbacks registered");

    // 3. 配置格式 - 默认 1280x720 YUV420P
    OH_AVFormat* format = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, 1280);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, 720);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_YUVI420);

    int32_t ret = OH_VideoDecoder_Configure(m_codec, format);
    OH_AVFormat_Destroy(format);

    if (ret != AV_ERR_OK) {
        OH_LOG_ERROR(LOG_APP, "MediaCodecAdapter: Failed to configure decoder, ret=%{public}d", ret);
        OH_VideoDecoder_Destroy(m_codec);
        m_codec = nullptr;
        return false;
    }
    OH_LOG_INFO(LOG_APP, "MediaCodecAdapter: decoder configured (1280x720 YUV420)");

    // 4. 启动解码器
    if (OH_VideoDecoder_Start(m_codec) != AV_ERR_OK) {
        OH_LOG_ERROR(LOG_APP, "MediaCodecAdapter: Failed to start decoder");
        OH_VideoDecoder_Destroy(m_codec);
        m_codec = nullptr;
        return false;
    }

    m_isRunning.store(true);
    m_isInitialized = true;
    OH_LOG_INFO(LOG_APP, "MediaCodecAdapter: initialized OK for %{public}s", mimeType.c_str());
    return true;
}

bool MediaCodecAdapter::QueueInput(const uint8_t* data, size_t size, int64_t pts) {
    if (!m_isRunning.load()) {
        OH_LOG_WARN(LOG_APP, "MediaCodecAdapter::QueueInput: decoder not running");
        return false;
    }

    // 将输入数据压入队列，OnNeedInputData 回调会取出并喂给编解码器
    InputPacket pkt;
    pkt.data.assign(data, data + size);
    pkt.pts = pts;

    {
        std::lock_guard<std::mutex> lk(m_inputMutex);
        m_inputQueue.push(std::move(pkt));
    }
    m_inputCv.notify_one(); // 通知回调线程有数据可用
    return true;
}

bool MediaCodecAdapter::DequeueOutput(std::vector<uint8_t>& outData, int64_t& outPts) {
    if (!m_isRunning.load()) return false;

    std::unique_lock<std::mutex> lk(m_outputMutex);
    // 等待输出数据（最多等 100ms）
    if (!m_outputCv.wait_for(lk, std::chrono::milliseconds(100), [this]() {
        return !m_outputQueue.empty() || m_outputEosReceived;
    })) {
        return false; // 超时无数据
    }

    if (m_outputQueue.empty()) return false;

    OutputFrame frame = std::move(m_outputQueue.front());
    m_outputQueue.pop();
    lk.unlock();

    outData = std::move(frame.data);
    outPts = frame.pts;
    return true;
}

void MediaCodecAdapter::Flush() {
    OH_LOG_INFO(LOG_APP, "MediaCodecAdapter::Flush()");
    if (m_codec) {
        OH_VideoDecoder_Flush(m_codec);
    }
    // 清空队列
    {
        std::lock_guard<std::mutex> lk(m_inputMutex);
        std::queue<InputPacket>().swap(m_inputQueue);
        m_inputEosSent = false;
    }
    {
        std::lock_guard<std::mutex> lk(m_outputMutex);
        std::queue<OutputFrame>().swap(m_outputQueue);
        m_outputEosReceived = false;
    }
    OH_LOG_INFO(LOG_APP, "MediaCodecAdapter: flushed & queues cleared");
}

void MediaCodecAdapter::Release() {
    OH_LOG_INFO(LOG_APP, "MediaCodecAdapter::Release()");
    m_isRunning.store(false);

    // 唤醒所有等待线程
    m_inputCv.notify_all();
    m_outputCv.notify_all();

    if (m_codec) {
        OH_VideoDecoder_Stop(m_codec);
        OH_VideoDecoder_Destroy(m_codec);
        m_codec = nullptr;
        OH_LOG_INFO(LOG_APP, "MediaCodecAdapter: decoder destroyed");
    }

    // 清空队列
    {
        std::lock_guard<std::mutex> lk(m_inputMutex);
        std::queue<InputPacket>().swap(m_inputQueue);
    }
    {
        std::lock_guard<std::mutex> lk(m_outputMutex);
        std::queue<OutputFrame>().swap(m_outputQueue);
    }

    m_isInitialized = false;
    m_inputEosSent = false;
    m_outputEosReceived = false;
    OH_LOG_INFO(LOG_APP, "MediaCodecAdapter: released");
}

// ============================================================
// 静态回调实现
// 这些回调在编解码器内部线程中调用，必须快速返回
// ============================================================

void MediaCodecAdapter::OnError(OH_AVCodec* codec, int32_t errorCode, void* userData) {
    MediaCodecAdapter* self = static_cast<MediaCodecAdapter*>(userData);
    if (!self) return;
    OH_LOG_ERROR(LOG_APP, "MediaCodecAdapter::OnError: code=%{public}d", errorCode);
    self->m_isRunning.store(false);
}

void MediaCodecAdapter::OnStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData) {
    MediaCodecAdapter* self = static_cast<MediaCodecAdapter*>(userData);
    if (!self || !format) return;

    // 读取新的分辨率信息
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &self->m_width);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &self->m_height);
    OH_LOG_INFO(LOG_APP, "MediaCodecAdapter::OnStreamChanged: new size %{public}d x %{public}d",
                self->m_width, self->m_height);
}

void MediaCodecAdapter::OnNeedInputData(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData) {
    MediaCodecAdapter* self = static_cast<MediaCodecAdapter*>(userData);
    if (!self || !self->m_isRunning.load() || !buffer) return;

    // 从输入队列取出一个 packet 填入缓冲区
    InputPacket pkt;
    {
        std::lock_guard<std::mutex> lk(self->m_inputMutex);
        if (!self->m_inputQueue.empty()) {
            pkt = std::move(self->m_inputQueue.front());
            self->m_inputQueue.pop();
        } else if (self->m_inputEosSent) {
            return; // 已经发送过 EOS
        } else {
            // 没有数据时传入 EOS（结束帧），通知解码器 flush 剩余数据
            OH_AVCodecBufferAttr attr;
            attr.pts = 0;
            attr.size = 0;
            attr.offset = 0;
            attr.flags = AVCODEC_BUFFER_FLAGS_EOS;
            OH_AVBuffer_SetBufferAttr(buffer, &attr);
            OH_VideoDecoder_PushInputBuffer(self->m_codec, index);
            self->m_inputEosSent = true;
            OH_LOG_INFO(LOG_APP, "MediaCodecAdapter: sent EOS (no pending input)");
            return;
        }
    }

    if (pkt.data.empty()) return;

    // 将数据拷贝到 AVBuffer (API 11+: OH_AVBuffer_GetAddr 直接返回 uint8_t*)
    uint8_t* dst = OH_AVBuffer_GetAddr(buffer);
    if (!dst) {
        OH_LOG_ERROR(LOG_APP, "MediaCodecAdapter: OH_AVBuffer_GetAddr returned null");
        return;
    }

    size_t copySize = pkt.data.size();

    if (copySize > 0) {
        memcpy(dst, pkt.data.data(), copySize);
    }

    // 设置缓冲区属性
    OH_AVCodecBufferAttr attr;
    attr.pts = pkt.pts;
    attr.size = static_cast<int32_t>(copySize);
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;

    // 设置缓冲区属性
    OH_AVBuffer_SetBufferAttr(buffer, &attr);

    // 推入解码器
    int32_t ret = OH_VideoDecoder_PushInputBuffer(self->m_codec, index);
    if (ret != AV_ERR_OK) {
        OH_LOG_ERROR(LOG_APP, "MediaCodecAdapter: PushInputBuffer failed, ret=%{public}d, index=%{public}u, size=%{public}d",
                     ret, index, static_cast<int32_t>(copySize));
    }
}

void MediaCodecAdapter::OnNeedOutputData(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData) {
    MediaCodecAdapter* self = static_cast<MediaCodecAdapter*>(userData);
    if (!self || !self->m_isRunning.load() || !buffer) return;

    // 获取输出缓冲区属性
    OH_AVCodecBufferAttr attr;
    if (OH_AVBuffer_GetBufferAttr(buffer, &attr) != AV_ERR_OK) {
        OH_LOG_ERROR(LOG_APP, "MediaCodecAdapter: GetBufferAttr failed for index=%{public}u", index);
        OH_VideoDecoder_FreeOutputBuffer(self->m_codec, index);
        return;
    }

    // 检查 EOS
    if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
        OH_LOG_INFO(LOG_APP, "MediaCodecAdapter: received EOS");
        {
            std::lock_guard<std::mutex> lk(self->m_outputMutex);
            self->m_outputEosReceived = true;
        }
        self->m_outputCv.notify_one();
        OH_VideoDecoder_FreeOutputBuffer(self->m_codec, index);
        return;
    }

    if (attr.size <= 0) {
        OH_VideoDecoder_FreeOutputBuffer(self->m_codec, index);
        return;
    }

    // 从输出缓冲区拷贝解码后的帧数据 (API 11+: OH_AVBuffer_GetAddr 直接返回 uint8_t*)
    uint8_t* src = OH_AVBuffer_GetAddr(buffer);
    if (!src) {
        OH_LOG_ERROR(LOG_APP, "MediaCodecAdapter: output OH_AVBuffer_GetAddr null");
        OH_VideoDecoder_FreeOutputBuffer(self->m_codec, index);
        return;
    }

    OutputFrame frame;
    frame.data.assign(src, src + attr.size);
    frame.pts = attr.pts;
    frame.flags = attr.flags;
    frame.width = self->m_width;
    frame.height = self->m_height;

    {
        std::lock_guard<std::mutex> lk(self->m_outputMutex);
        // 限制输出队列大小，防止内存暴涨
        if (self->m_outputQueue.size() < 60) {  // 最多缓存 60 帧 (~2s @30fps)
            self->m_outputQueue.push(std::move(frame));
        } else {
            OH_LOG_WARN(LOG_APP, "MediaCodecAdapter: output queue full, dropping frame");
        }
    }
    self->m_outputCv.notify_one();

    // 释放输出缓冲区，让编解码器继续工作
    OH_VideoDecoder_FreeOutputBuffer(self->m_codec, index);
}

// ============================================================
// 工厂函数
// ============================================================
std::unique_ptr<IHardwareCodec> CreateHardwareCodec() {
    return std::make_unique<MediaCodecAdapter>();
}
