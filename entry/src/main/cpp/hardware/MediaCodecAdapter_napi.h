#ifndef MEDIA_CODEC_ADAPTER_NAPI_H
#define MEDIA_CODEC_ADAPTER_NAPI_H

#include "napi/native_api.h"

/**
 * @brief 硬件编解码器 NAPI 接口
 * 
 * 这些函数在 MediaCodecAdapter_napi.cpp 中实现，
 * 通过 napi_init.cpp 注册到模块导出表。
 */

// 创建编解码器实例 -> 返回 external 对象
napi_value CreateHardwareCodec(napi_env env, napi_callback_info info);

// 初始化编解码器: (codec, mimeType, isEncoder) -> boolean
napi_value InitHardwareCodec(napi_env env, napi_callback_info info);

// 喂入输入数据: (codec, ArrayBuffer, pts) -> boolean
napi_value QueueInputData(napi_env env, napi_callback_info info);

// 取出解码输出: (codec) -> { data: ArrayBuffer, pts: number, ok: boolean }
napi_value DequeueOutputData(napi_env env, napi_callback_info info);

// 释放编解码器: (codec) -> void
napi_value ReleaseHardwareCodec(napi_env env, napi_callback_info info);

#endif // MEDIA_CODEC_ADAPTER_NAPI_H
