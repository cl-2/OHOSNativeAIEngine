#include "napi/native_api.h"
#include "hilog/log.h"
#include "hardware/MediaCodecAdapter.h"
#include "hardware/IHardwareCodec.h"
#include "hardware/MediaCodecAdapter_napi.h"
#include <cstring>

#define LOG_TAG "MediaCodecNAPI"
#define LOG_DOMAIN 0x0200

// Finalizer to delete the codec instance
static void CodecFinalizer(napi_env env, void* data, void* hint) {
    IHardwareCodec* codec = static_cast<IHardwareCodec*>(data);
    if (codec) {
        codec->Release();
        delete codec;
    }
}

napi_value CreateHardwareCodec(napi_env env, napi_callback_info info) {
    napi_value result;
    IHardwareCodec* codec = CreateHardwareCodec().release(); // factory returns unique_ptr
    napi_status status = napi_create_external(env, codec, CodecFinalizer, nullptr, &result);
    if (status != napi_ok) {
        napi_throw_error(env, nullptr, "Failed to create external for codec");
        return nullptr;
    }
    return result;
}

napi_value InitHardwareCodec(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        napi_throw_error(env, nullptr, "InitHardwareCodec requires 3 arguments");
        return nullptr;
    }
    // arg0: external codec
    IHardwareCodec* codec = nullptr;
    napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&codec));
    // arg1: mimeType string
    size_t mimeLen;
    napi_get_value_string_utf8(env, argv[1], nullptr, 0, &mimeLen);
    std::string mime(mimeLen, '\0');
    napi_get_value_string_utf8(env, argv[1], &mime[0], mimeLen + 1, &mimeLen);
    mime.resize(mimeLen);
    // arg2: isEncoder bool
    bool isEncoder;
    napi_get_value_bool(env, argv[2], &isEncoder);
    bool ok = codec->Init(mime, isEncoder);
    napi_value ret;
    napi_get_boolean(env, ok, &ret);
    return ret;
}

napi_value QueueInputData(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        napi_throw_error(env, nullptr, "QueueInputData requires 3 arguments");
        return nullptr;
    }
    IHardwareCodec* codec = nullptr;
    napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&codec));
    // arg1: ArrayBuffer (or Uint8Array)
    void* data = nullptr;
    size_t dataLen = 0;
    napi_get_arraybuffer_info(env, argv[1], &data, &dataLen);
    // arg2: int64 pts
    int64_t pts;
    napi_get_value_int64(env, argv[2], &pts);
    bool ok = codec->QueueInput(static_cast<uint8_t*>(data), dataLen, pts);
    napi_value ret;
    napi_get_boolean(env, ok, &ret);
    return ret;
}

napi_value DequeueOutputData(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "DequeueOutputData requires 1 argument");
        return nullptr;
    }
    IHardwareCodec* codec = nullptr;
    napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&codec));
    std::vector<uint8_t> outData;
    int64_t outPts = 0;
    bool ok = codec->DequeueOutput(outData, outPts);
    napi_value resultObj;
    napi_create_object(env, &resultObj);
    // data buffer
    napi_value buffer;
    void* bufPtr = nullptr;
    napi_create_arraybuffer(env, outData.size(), &bufPtr, &buffer);
    if (!outData.empty()) {
        memcpy(bufPtr, outData.data(), outData.size());
    }
    napi_set_named_property(env, resultObj, "data", buffer);
    // pts
    napi_value ptsVal;
    napi_create_int64(env, outPts, &ptsVal);
    napi_set_named_property(env, resultObj, "pts", ptsVal);
    // success flag
    napi_value success;
    napi_get_boolean(env, ok, &success);
    napi_set_named_property(env, resultObj, "ok", success);
    return resultObj;
}

napi_value ReleaseHardwareCodec(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "ReleaseHardwareCodec requires 1 argument");
        return nullptr;
    }
    IHardwareCodec* codec = nullptr;
    napi_get_value_external(env, argv[0], reinterpret_cast<void**>(&codec));
    if (codec) {
        codec->Release();
        delete codec;
    }
    napi_value ret;
    napi_get_undefined(env, &ret);
    return ret;
}

// Export registration will be added in napi_init.cpp
