#ifndef IHARDWARE_CODEC_H
#define IHARDWARE_CODEC_H

#include <vector>
#include <string>
#include <memory>

/**
 * @brief 硬件编解码器统一接口 (HAL 层)
 * 用于展示跨平台 (HarmonyOS MediaCodec / Windows DXVA2) 的抽象能力
 */
class IHardwareCodec {
public:
    virtual ~IHardwareCodec() = default;

    // 初始化编解码器
    virtual bool Init(const std::string& mimeType, bool isEncoder) = 0;

    // 输入原始数据 (PCM / YUV)
    virtual bool QueueInput(const uint8_t* data, size_t size, int64_t pts) = 0;

    // 获取输出数据 (Encoded Bitstream / Decoded Frames)
    virtual bool DequeueOutput(std::vector<uint8_t>& outData, int64_t& outPts) = 0;

    // 刷新缓冲区
    virtual void Flush() = 0;

    // 释放资源
    virtual void Release() = 0;

    // 获取当前组件名称 (如 "MediaCodec_H26x")
    virtual std::string GetName() const = 0;
};

// 简单工厂/工厂方法声明 (具体实现在对应的 cpp 中)
std::unique_ptr<IHardwareCodec> CreateHardwareCodec();

#endif // IHARDWARE_CODEC_H
