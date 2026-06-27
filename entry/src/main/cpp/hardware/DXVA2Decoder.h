#ifndef DXVA2_DECODER_H
#define DXVA2_DECODER_H

#include "IHardwareCodec.h"

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxva2api.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <codecapi.h>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <thread>
#include <atomic>
#endif

/**
 * @brief Windows DXVA2 硬件加速解码器适配器
 * 
 * 使用 DirectX Video Acceleration (DXVA2) API 实现 H.264/HEVC 硬件解码。
 * 支持 Direct3D 11 管线，降低 CPU 占用。
 * 
 * 工作流程：
 * 1. 创建 D3D11 设备和 DXGI 管理器
 * 2. 使用 Media Foundation 的 DXVA2 解码器
 * 3. 输入压缩视频数据 → 输出解码后的 YUV 帧
 */
class DXVA2Decoder : public IHardwareCodec {
public:
    DXVA2Decoder();
    virtual ~DXVA2Decoder();

    bool Init(const std::string& mimeType, bool isEncoder) override;
    bool QueueInput(const uint8_t* data, size_t size, int64_t pts) override;
    bool DequeueOutput(std::vector<uint8_t>& outData, int64_t& outPts) override;
    void Flush() override;
    void Release() override;
    std::string GetName() const override { return "Windows_DXVA2"; }

private:
#ifdef _WIN32
    // D3D11 资源
    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    IDXGIDeviceManager* m_dxgiManager = nullptr;
    HANDLE m_dxgiManagerHandle = nullptr;

    // Media Foundation 解码器
    IMFTransform* m_decoder = nullptr;
    IMFMediaType* m_inputType = nullptr;
    IMFMediaType* m_outputType = nullptr;

    // 输入/输出队列
    struct InputSample {
        std::vector<uint8_t> data;
        int64_t pts;
        bool isEOS = false;
    };
    struct OutputSample {
        std::vector<uint8_t> data;
        int64_t pts;
        int32_t width = 0;
        int32_t height = 0;
    };

    std::mutex m_inputMutex;
    std::queue<InputSample> m_inputQueue;
    std::condition_variable m_inputCv;

    std::mutex m_outputMutex;
    std::queue<OutputSample> m_outputQueue;
    std::condition_variable m_outputCv;

    // GUID 映射 (MIME → DXVA2 GUID)
    GUID GetDecoderGUID(const std::string& mimeType);
    
    // 解码线程
    void DecodeLoop();

    // 初始化 D3D11
    bool InitD3D11();
#endif

    std::atomic<bool> m_isRunning{false};
    std::string m_mimeType;
    bool m_isInitialized = false;
};

#endif // DXVA2_DECODER_H
