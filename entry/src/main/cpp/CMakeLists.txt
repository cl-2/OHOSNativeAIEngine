cmake_minimum_required(VERSION 3.5.0)
project(OHOSNativeAIEngine C CXX)

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
    utils/CancelScope.h
    audio/AudioStateMachine.cpp
    audio/Aec.cpp
    audio/NoiseSuppressor.cpp
    audio/PipelineTypes.h
    metrics/MetricsCollector.cpp
    metrics/BenchmarkEngine.cpp
    llm/LocalLlmEngine.cpp
    llm/GgufLlmEngine.cpp
    llm/RemoteLlmEngine.cpp
    llm/BpeTokenizer.cpp
)

option(OHOS_GGML_CPU_VARIANTS
    "Experiment: package baseline/dotprod/fp16 GGML CPU backends and select at runtime"
    OFF)
set(OHOS_GGML_CPU_VARIANTS_ACTIVE OFF)
if(OHOS_GGML_CPU_VARIANTS AND OHOS_ARCH STREQUAL "arm64-v8a")
    set(OHOS_GGML_CPU_VARIANTS_ACTIVE ON)
endif()

if(OHOS_GGML_CPU_VARIANTS_ACTIVE)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(GGML_BACKEND_DL ON CACHE BOOL "" FORCE)
    set(GGML_CPU_ALL_VARIANTS ON CACHE BOOL "" FORCE)
    target_compile_definitions(native_lib PRIVATE OHOS_GGML_CPU_VARIANTS=1)
else()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
    set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "" FORCE)
endif()
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_LLAMAFILE OFF CACHE BOOL "" FORCE)
set(GGML_CPU_REPACK OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
# Explicit A/B switch. The OFF branch must force-clear GGML_CPU_ARM_ARCH:
# removing a set(... CACHE ...) line does not remove its old CMakeCache value.
option(OHOS_GGML_ARM_DOTPROD "Use armv8.2 dotprod/fp16 for GGML CPU" OFF)
if(OHOS_GGML_CPU_VARIANTS_ACTIVE)
    # Each dynamic backend owns its architecture flags. A global ARM target
    # would make even the baseline/shared core unsafe on older devices.
    set(GGML_CPU_ARM_ARCH "" CACHE STRING "" FORCE)
elseif(OHOS_GGML_ARM_DOTPROD AND OHOS_ARCH STREQUAL "arm64-v8a")
    set(GGML_CPU_ARM_ARCH "armv8.2-a+dotprod+fp16" CACHE STRING "" FORCE)
else()
    set(GGML_CPU_ARM_ARCH "" CACHE STRING "" FORCE)
endif()
set(LLAMA_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../../cmake/ApplyLlamaPatch.cmake)
add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
# Static builds expose ggml-cpu. The runtime-dispatch build exposes one
# differently named MODULE target per ARM feature tier, so they must receive
# the same release-grade optimization explicitly even in a Debug HAP.
foreach(LLAMA_TARGET
        llama
        ggml
        ggml-base
        ggml-cpu
        ggml-cpu-armv8.0_1
        ggml-cpu-armv8.2_1
        ggml-cpu-armv8.2_2)
    if(TARGET ${LLAMA_TARGET})
        target_compile_options(${LLAMA_TARGET} PRIVATE -O3 -g0)
    endif()
endforeach()

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
target_link_libraries(native_lib PRIVATE llama)
