#include "EngineBridge.h"
#include <QFile>
#include <QStringList>

EngineBridge& EngineBridge::instance() {
    static EngineBridge bridge;
    return bridge;
}

bool EngineBridge::load(const QString& dllPath) {
    if (m_loaded) return true;

    // 搜索策略: 1) 指定路径  2) 同目录  3) PATH 环境变量
    QStringList searchPaths;
    if (!dllPath.isEmpty()) searchPaths << dllPath;
    searchPaths << "native_ai.dll" << "./native_ai.dll"
                << "./build/windows/platforms/windows/native_ai.dll"
                << "../build/windows/platforms/windows/native_ai.dll";

    for (const auto& path : searchPaths) {
        if (QFile::exists(path)) {
            m_lib = new QLibrary(path);
            if (m_lib->load()) {
                m_loaded = true;
                qInfo() << "[EngineBridge] DLL loaded:" << path;
                return true;
            }
            m_lastError = m_lib->errorString();
            delete m_lib;
            m_lib = nullptr;
        }
    }

    m_lastError = "native_ai.dll not found in any search path";
    qWarning() << "[EngineBridge] Failed to load DLL:" << m_lastError;
    return false;
}

void EngineBridge::unload() {
    if (m_lib) {
        destroy();  // 确保 DLL 内部资源释放
        m_lib->unload();
        delete m_lib;
        m_lib = nullptr;
    }
    m_loaded = false;
}

// ========== 包装函数 ==========

const char* EngineBridge::getVersion() {
    static const char* fallback = "Engine not loaded";
    auto fn = resolve<const char*(*)()>("AI_GetVersion");
    return fn ? fn() : fallback;
}

bool EngineBridge::init(const char* modelsDir) {
    auto fn = resolve<bool(*)(const char*)>("AI_Init");
    return fn ? fn(modelsDir) : false;
}

void EngineBridge::destroy() {
    auto fn = resolve<void(*)()>("AI_Destroy");
    if (fn) fn();
}

// --- ASR ---
bool EngineBridge::startASR() {
    auto fn = resolve<bool(*)()>("AI_StartASR");
    return fn ? fn() : false;
}
void EngineBridge::feedAudio(const short* pcm, int numSamples) {
    auto fn = resolve<void(*)(const short*, int)>("AI_FeedAudio");
    if (fn) fn(pcm, numSamples);
}
void EngineBridge::stopASR() {
    auto fn = resolve<void(*)()>("AI_StopASR");
    if (fn) fn();
}
bool EngineBridge::isAsrRunning() {
    auto fn = resolve<bool(*)()>("AI_IsAsrRunning");
    return fn ? fn() : false;
}
void EngineBridge::setAsrCallback(AsrResultCallback cb) {
    auto fn = resolve<void(*)(AsrResultCallback)>("AI_SetAsrCallback");
    if (fn) fn(cb);
}
void EngineBridge::setPerfCallback(PerfCallback cb) {
    auto fn = resolve<void(*)(PerfCallback)>("AI_SetPerfCallback");
    if (fn) fn(cb);
}

// --- TTS ---
bool EngineBridge::startTTS(const char* text, float speed) {
    auto fn = resolve<bool(*)(const char*, float)>("AI_StartTTS");
    if (!fn) return false;
    return fn(text, speed);
}
void EngineBridge::stopTTS() {
    auto fn = resolve<void(*)()>("AI_StopTTS");
    if (fn) fn();
}
bool EngineBridge::isTtsBusy() {
    auto fn = resolve<bool(*)()>("AI_IsTtsBusy");
    return fn ? fn() : false;
}
void EngineBridge::setTtsCallback(TtsAudioCallback cb) {
    auto fn = resolve<void(*)(TtsAudioCallback)>("AI_SetTtsCallback");
    if (fn) fn(cb);
}

// --- Supertonic TTS ---
bool EngineBridge::initSupertonic(const char* modelDir, const char* voiceStyle) {
    auto fn = resolve<bool(*)(const char*, const char*)>("AI_InitSupertonic");
    return fn ? fn(modelDir, voiceStyle) : false;
}
bool EngineBridge::setSupertonicVoice(const char* voiceName) {
    auto fn = resolve<bool(*)(const char*)>("AI_SetSupertonicVoice");
    return fn ? fn(voiceName) : false;
}
int EngineBridge::getSupertonicVoiceCount() {
    auto fn = resolve<int(*)()>("AI_GetSupertonicVoiceCount");
    return fn ? fn() : 0;
}
const char* EngineBridge::getSupertonicVoiceName(int index) {
    static const char* empty = "";
    auto fn = resolve<const char*(*)(int)>("AI_GetSupertonicVoiceName");
    return fn ? fn(index) : empty;
}
bool EngineBridge::supertonicIsReady() {
    auto fn = resolve<bool(*)()>("AI_SupertonicIsReady");
    return fn ? fn() : false;
}

// --- LLM ---
void EngineBridge::sendLlmMessage(const char* text) {
    auto fn = resolve<void(*)(const char*)>("AI_SendLlmMessage");
    if (fn) fn(text);
}
void EngineBridge::sendLlmMessageStream(const char* text) {
    auto fn = resolve<void(*)(const char*)>("AI_SendLlmMessageStream");
    if (fn) fn(text);
}
void EngineBridge::stopLlm() {
    auto fn = resolve<void(*)()>("AI_StopLlm");
    if (fn) fn();
}
void EngineBridge::clearLlmHistory() {
    auto fn = resolve<void(*)()>("AI_ClearLlmHistory");
    if (fn) fn();
}
void EngineBridge::setLlmConfig(const char* apiUrl, const char* apiKey, const char* modelName) {
    auto fn = resolve<void(*)(const char*, const char*, const char*)>("AI_SetLlmConfig");
    if (fn) fn(apiUrl, apiKey, modelName);
}
void EngineBridge::setLlmCallbacks(LlmResponseCallback onResponse, LlmStreamCallback onStream, LlmErrorCallback onError) {
    auto fn = resolve<void(*)(LlmResponseCallback, LlmStreamCallback, LlmErrorCallback)>("AI_SetLlmCallbacks");
    if (fn) fn(onResponse, onStream, onError);
}

// --- Audio capture ---
bool EngineBridge::startCapture(int sampleRate) {
    auto fn = resolve<bool(*)(int)>("AI_StartCapture");
    return fn ? fn(sampleRate) : false;
}
void EngineBridge::stopCapture() {
    auto fn = resolve<void(*)()>("AI_StopCapture");
    if (fn) fn();
}
bool EngineBridge::isCapturing() {
    auto fn = resolve<bool(*)()>("AI_IsCapturing");
    return fn ? fn() : false;
}

// --- Audio playback ---
void EngineBridge::playAudio(const float* audio, int numSamples, int sampleRate) {
    auto fn = resolve<void(*)(const float*, int, int)>("AI_PlayAudio");
    if (fn) fn(audio, numSamples, sampleRate);
}
void EngineBridge::stopPlayback() {
    auto fn = resolve<void(*)()>("AI_StopPlayback");
    if (fn) fn();
}
bool EngineBridge::isPlaying() {
    auto fn = resolve<bool(*)()>("AI_IsPlaying");
    return fn ? fn() : false;
}

// --- Full duplex ---
void EngineBridge::startFullDuplex() {
    auto fn = resolve<void(*)()>("AI_StartFullDuplex");
    if (fn) fn();
}
void EngineBridge::stopFullDuplex() {
    auto fn = resolve<void(*)()>("AI_StopFullDuplex");
    if (fn) fn();
}
const char* EngineBridge::getState() {
    static const char* idle = "IDLE";
    auto fn = resolve<const char*(*)()>("AI_GetState");
    return fn ? fn() : idle;
}

// --- Hardware ---
void* EngineBridge::createHardwareCodec() {
    auto fn = resolve<void*(*)()>("AI_CreateHardwareCodec");
    return fn ? fn() : nullptr;
}
bool EngineBridge::initHardwareCodec(void* codec, const char* mimeType, bool isEncoder) {
    auto fn = resolve<bool(*)(void*, const char*, bool)>("AI_InitHardwareCodec");
    return fn ? fn(codec, mimeType, isEncoder) : false;
}
bool EngineBridge::codecQueueInput(void* codec, const unsigned char* data, int size, long long pts) {
    auto fn = resolve<bool(*)(void*, const unsigned char*, int, long long)>("AI_CodecQueueInput");
    return fn ? fn(codec, data, size, pts) : false;
}
void* EngineBridge::codecDequeueOutput(void* codec, int* outSize, long long* outPts) {
    auto fn = resolve<void*(*)(void*, int*, long long*)>("AI_CodecDequeueOutput");
    return fn ? fn(codec, outSize, outPts) : nullptr;
}
void EngineBridge::releaseHardwareCodec(void* codec) {
    auto fn = resolve<void(*)(void*)>("AI_ReleaseHardwareCodec");
    if (fn) fn(codec);
}

// --- Util ---
void EngineBridge::setLogLevel(int level) {
    auto fn = resolve<void(*)(int)>("AI_SetLogLevel");
    if (fn) fn(level);
}
