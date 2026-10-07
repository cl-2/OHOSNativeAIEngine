#include "DXVA2Decoder.h"
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <iostream>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#endif

#ifndef LOG_APP
#define LOG_APP 0
#endif

// 简易 Windows 日志 (无 hilog 时使用)
#ifdef _WIN32
#define DXVA_LOG_INFO(fmt, ...) printf("[DXVA2] " fmt "\n", ##__VA_ARGS__)
#define DXVA_LOG_ERROR(fmt, ...) fprintf(stderr, "[DXVA2 ERROR] " fmt "\n", ##__VA_ARGS__)
#else
#define DXVA_LOG_INFO(fmt, ...)
#define DXVA_LOG_ERROR(fmt, ...)
#endif

// ============================================================
// DXVA2Decoder - Windows DirectX VA 2 硬件解码适配器
// 使用 Media Foundation + D3D11 实现 GPU 加速解码
// ============================================================

DXVA2Decoder::DXVA2Decoder()
    : m_isInitialized(false)
    , m_isRunning(false)
{
}

DXVA2Decoder::~DXVA2Decoder() {
    DXVA2Decoder::Release();
}

bool DXVA2Decoder::InitD3D11() {
#ifdef _WIN32
    HRESULT hr;

    // 1. 创建 D3D11 设备 (使用硬件加速)
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selectedLevel;

    hr = D3D11CreateDevice(
        nullptr,                    // 使用默认适配器
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // 不使用软件光栅化
        flags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_d3dDevice,
        &selectedLevel,
        &m_d3dContext
    );

    if (FAILED(hr)) {
        DXVA_LOG_ERROR("D3D11CreateDevice failed: hr=0x%08X", hr);
        return false;
    }
    DXVA_LOG_INFO("D3D11 device created (feature level: 0x%04X)", selectedLevel);

    // 2. 查询 DXGI 设备
    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) {
        DXVA_LOG_ERROR("QueryInterface IDXGIDevice failed: hr=0x%08X", hr);
        return false;
    }

    // 3. 创建 DXGI 设备管理器
    hr = MFCreateDXGIDeviceManager(&m_dxgiManagerHandle, &m_dxgiManager);
    if (FAILED(hr)) {
        DXVA_LOG_ERROR("MFCreateDXGIDeviceManager failed: hr=0x%08X", hr);
        dxgiDevice->Release();
        return false;
    }

    // 4. 将 D3D11 设备注册到 DXGI 管理器
    hr = m_dxgiManager->ResetDevice(m_d3dDevice, m_dxgiManagerHandle);
    if (FAILED(hr)) {
        DXVA_LOG_ERROR("DXGI ResetDevice failed: hr=0x%08X", hr);
        dxgiDevice->Release();
        return false;
    }

    dxgiDevice->Release();
    DXVA_LOG_INFO("D3D11 + DXGI initialized OK");
    return true;
#else
    return false;
#endif
}

GUID DXVA2Decoder::GetDecoderGUID(const std::string& mimeType) {
#ifdef _WIN32
    if (mimeType == "video/avc" || mimeType == "video/h264" || mimeType == "h264") {
        return MFVideoFormat_H264;
    }
    if (mimeType == "video/hevc" || mimeType == "video/h265" || mimeType == "h265") {
        return MFVideoFormat_HEVC;
    }
    if (mimeType == "video/vp9") {
        return MFVideoFormat_VP90;
    }
    if (mimeType == "video/av1") {
        return MFVideoFormat_AV1;
    }
    // 默认 H.264
    DXVA_LOG_WARN("Unknown mime type '%s', defaulting to H.264", mimeType.c_str());
    return MFVideoFormat_H264;
#else
    (void)mimeType;
    return GUID();
#endif
}

bool DXVA2Decoder::Init(const std::string& mimeType, bool isEncoder) {
    if (m_isInitialized) return true;

    m_mimeType = mimeType;

#ifdef _WIN32
    // 仅支持解码器
    if (isEncoder) {
        DXVA_LOG_ERROR("Encoder mode not supported yet");
        return false;
    }

    // 1. 初始化 Media Foundation
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        DXVA_LOG_ERROR("MFStartup failed: hr=0x%08X", hr);
        return false;
    }

    // 2. 初始化 D3D11
    if (!InitD3D11()) {
        DXVA_LOG_ERROR("D3D11 init failed, falling back to software decode");
        // 不立即失败 — 允许回退到软件解码
    }

    // 3. 创建 Media Foundation DXVA2 解码器
    GUID decoderGUID = GetDecoderGUID(mimeType);
    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Video, decoderGUID };

    IMFActivate** activates = nullptr;
    UINT32 numActivates = 0;

    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputInfo,
        nullptr,  // 不限制输出类型
        &activates,
        &numActivates
    );

    if (FAILED(hr) || numActivates == 0) {
        DXVA_LOG_INFO("No HW decoder found, trying software decoder");
        // 回退到软件解码器
        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputInfo,
            nullptr,
            &activates,
            &numActivates
        );
    }

    if (FAILED(hr) || numActivates == 0) {
        DXVA_LOG_ERROR("No decoder found for mime: %s", mimeType.c_str());
        MFShutdown();
        return false;
    }

    // 激活第一个解码器
    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_decoder));
    if (FAILED(hr)) {
        DXVA_LOG_ERROR("Failed to activate decoder: hr=0x%08X", hr);
        for (UINT32 i = 0; i < numActivates; i++) activates[i]->Release();
        CoTaskMemFree(activates);
        MFShutdown();
        return false;
    }

    // 清理激活对象
    for (UINT32 i = 0; i < numActivates; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    // 4. 设置 DXGI 管理器 (启用硬件加速)
    if (m_dxgiManager) {
        hr = m_decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                        reinterpret_cast<ULONG_PTR>(m_dxgiManager));
        if (SUCCEEDED(hr)) {
            DXVA_LOG_INFO("DXVA2 HW acceleration enabled via D3D11");
        } else {
            DXVA_LOG_INFO("DXVA2 HW acceleration not available, using software: hr=0x%08X", hr);
        }
    }

    // 5. 设置输入类型
    hr = MFCreateMediaType(&m_inputType);
    if (SUCCEEDED(hr)) {
        m_inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        m_inputType->SetGUID(MF_MT_SUBTYPE, decoderGUID);
        m_inputType->SetUINT32(MF_MT_FRAME_SIZE, (1280 << 16) | 720);  // 1280x720
        hr = m_decoder->SetInputType(0, m_inputType, 0);
        if (FAILED(hr)) {
            DXVA_LOG_ERROR("SetInputType failed: hr=0x%08X", hr);
        }
    }

    // 6. 设置输出类型
    GUID outputSubtype = MFVideoFormat_NV12;  // DXVA2 常用输出格式
    hr = MFCreateMediaType(&m_outputType);
    if (SUCCEEDED(hr)) {
        m_outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        m_outputType->SetGUID(MF_MT_SUBTYPE, outputSubtype);
        m_outputType->SetUINT32(MF_MT_FRAME_SIZE, (1280 << 16) | 720);
        hr = m_decoder->SetOutputType(0, m_outputType, 0);
        if (FAILED(hr)) {
            DXVA_LOG_ERROR("SetOutputType failed: hr=0x%08X", hr);
        }
    }

    // 7. 获取完整输出类型
    if (m_outputType) {
        hr = m_decoder->GetOutputType(0, &m_outputType);
        if (FAILED(hr)) {
            DXVA_LOG_ERROR("GetOutputType failed: hr=0x%08X", hr);
        }
    }

    m_isRunning.store(true);
    m_isInitialized = true;
    DXVA_LOG_INFO("DXVA2 decoder initialized: %s", mimeType.c_str());
    return true;
#else
    (void)mimeType;
    (void)isEncoder;
    return false;
#endif
}

bool DXVA2Decoder::QueueInput(const uint8_t* data, size_t size, int64_t pts) {
    if (!m_isRunning.load()) return false;

#ifdef _WIN32
    InputSample sample{};
    sample.data.assign(data, data + size);
    sample.pts = pts;

    {
        std::lock_guard<std::mutex> lk(m_inputMutex);
        m_inputQueue.push(std::move(sample));
    }
    m_inputCv.notify_one();
    return true;
#else
    (void)data;
    (void)size;
    (void)pts;
    return false;
#endif
}

bool DXVA2Decoder::DequeueOutput(std::vector<uint8_t>& outData, int64_t& outPts) {
    if (!m_isRunning.load()) return false;

#ifdef _WIN32
    std::unique_lock<std::mutex> lk(m_outputMutex);
    if (m_outputCv.wait_for(lk, std::chrono::milliseconds(100), [this]() {
        return !m_outputQueue.empty();
    })) {
        if (m_outputQueue.empty()) return false;
        OutputSample sample = std::move(m_outputQueue.front());
        m_outputQueue.pop();
        lk.unlock();

        outData = std::move(sample.data);
        outPts = sample.pts;
        return true;
    }
    return false;
#else
    (void)outData;
    (void)outPts;
    return false;
#endif
}

void DXVA2Decoder::Flush() {
#ifdef _WIN32
    DXVA_LOG_INFO("Flush");
    if (m_decoder) {
        m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    {
        std::lock_guard<std::mutex> lk(m_inputMutex);
        std::queue<InputSample>().swap(m_inputQueue);
    }
    {
        std::lock_guard<std::mutex> lk(m_outputMutex);
        std::queue<OutputSample>().swap(m_outputQueue);
    }
#endif
}

void DXVA2Decoder::Release() {
#ifdef _WIN32
    DXVA_LOG_INFO("Release");
    m_isRunning.store(false);

    m_inputCv.notify_all();
    m_outputCv.notify_all();

    // 清空队列
    {
        std::lock_guard<std::mutex> lk(m_inputMutex);
        std::queue<InputSample>().swap(m_inputQueue);
    }
    {
        std::lock_guard<std::mutex> lk(m_outputMutex);
        std::queue<OutputSample>().swap(m_outputQueue);
    }

    // 释放 Media Foundation 资源
    if (m_outputType) { m_outputType->Release(); m_outputType = nullptr; }
    if (m_inputType) { m_inputType->Release(); m_inputType = nullptr; }
    if (m_decoder) { m_decoder->Release(); m_decoder = nullptr; }
    if (m_dxgiManager) { m_dxgiManager->Release(); m_dxgiManager = nullptr; }
    if (m_d3dContext) { m_d3dContext->Release(); m_d3dContext = nullptr; }
    if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }

    MFShutdown();
    DXVA_LOG_INFO("DXVA2 decoder released");
#endif

    m_isInitialized = false;
}

// Windows 平台工厂函数
#ifdef _WIN32
std::unique_ptr<IHardwareCodec> CreateHardwareCodec() {
    return std::make_unique<DXVA2Decoder>();
}
#endif
