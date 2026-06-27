cmake_minimum_required(VERSION 3.5.0)
set(CMAKE_C_COMPILER "D:/deveco/DevEco Studio/sdk/default/hms/native/BiSheng/bin/clang.exe")
set(CMAKE_CXX_COMPILER "D:/deveco/DevEco Studio/sdk/default/hms/native/BiSheng/bin/clang++.exe")

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

# 1. 处理包查找 (保持原有逻辑)
if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# 2. 添加源文件 (包含硬件加速层、基准测试、Supertonic TTS引擎和AEC回声消除)
add_library(native_lib SHARED 
    napi_init.cpp
    hardware/MediaCodecAdapter.cpp
    hardware/MediaCodecAdapter_napi.cpp
    hardware/CodecBenchmark.cpp
    tts/SupertonicTtsEngine.cpp
    audio/AudioStateMachine.cpp
    audio/Aec.cpp
    metrics/MetricsCollector.cpp
    llm/LocalLlmEngine.cpp
    llm/RemoteLlmEngine.cpp
)

# 3. Debug 模式：关闭优化、保留调试符号，使断点能准确命中
target_compile_options(native_lib PRIVATE -O0 -g)

# 4. 设置头文件包含目录
include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/include
    ${NATIVERENDER_ROOT_PATH}/include/onnxruntime
)

# 5. 使用 IMPORTED 目标链接 sherpa-onnx-c-api
# IMPORTED 库不会被 CMake 复制到构建输出，避免与 entry/libs/ 中的同名 so 冲突
add_library(sherpa-onnx-c-api SHARED IMPORTED)
set_target_properties(sherpa-onnx-c-api PROPERTIES
    IMPORTED_LOCATION ${NATIVERENDER_ROOT_PATH}/libs/${OHOS_ARCH}/libsherpa-onnx-c-api.so
    IMPORTED_NO_SONAME TRUE
)

target_link_libraries(native_lib PUBLIC
    ace_napi.z          # NAPI 库
    hilog_ndk.z         # OpenHarmony 日志库
    native_media_codecbase # 媒体编解码基础库
    native_media_vdec      # 视频解码库 (鸿蒙 API 10+)
    native_media_core      # AVFormat 核心结构体所在的库
    dl                  # dlopen/dlsym
    m                   # 数学库
    rawfile.z           # Native RawFile API
)
target_link_libraries(native_lib PRIVATE sherpa-onnx-c-api)