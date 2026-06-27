# ============================================================
# 跨平台 AI Engine 顶层 CMakeLists.txt
# 支持 HarmonyOS (NAPI) + Windows (DLL) 双平台构建
# ============================================================
#
# 使用方法:
#   # Windows 原生编译 (需要先安装依赖)
#   cmake -B build/windows -G "Visual Studio 17 2022" -A x64 -DPLATFORM=WINDOWS
#   cmake --build build/windows --config Release
#
#   # HarmonyOS 交叉编译 (需要 OHOS SDK)
#   cmake -B build/harmony -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-harmonyos.cmake -DPLATFORM=OHOS
#   cmake --build build/harmony
# ============================================================

cmake_minimum_required(VERSION 3.16)
project(native_ai_engine
    VERSION 1.0.0
    DESCRIPTION "Cross-platform AI Engine with HW acceleration"
    LANGUAGES C CXX
)

# ============================================================
# 平台检测
# ============================================================
if(NOT DEFINED PLATFORM)
    if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
        set(PLATFORM "OHOS")
    elseif(WIN32)
        set(PLATFORM "WINDOWS")
    else()
        message(FATAL_ERROR "Unsupported platform. Set -DPLATFORM=OHOS or -DPLATFORM=WINDOWS")
    endif()
endif()

message(STATUS "Building for platform: ${PLATFORM}")

# ============================================================
# 全局编译选项
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================
# 跨平台核心引擎库 (src/)
# ============================================================
add_subdirectory(src)

# ============================================================
# 平台相关构建
# ============================================================
if(PLATFORM STREQUAL "OHOS")
    # --- HarmonyOS 构建 (native_lib.so) ---
    add_library(native_lib SHARED
        entry/src/main/cpp/napi_init.cpp
        entry/src/main/cpp/hardware/MediaCodecAdapter.h
        entry/src/main/cpp/hardware/MediaCodecAdapter.cpp
        entry/src/main/cpp/hardware/MediaCodecAdapter_napi.h
        entry/src/main/cpp/hardware/MediaCodecAdapter_napi.cpp
        entry/src/main/cpp/hardware/CodecBenchmark.h
        entry/src/main/cpp/hardware/CodecBenchmark.cpp
        entry/src/main/cpp/tts/SupertonicTtsEngine.h
        entry/src/main/cpp/tts/SupertonicTtsEngine.cpp
        entry/src/main/cpp/audio/AudioStateMachine.h
        entry/src/main/cpp/audio/AudioStateMachine.cpp
    )

    target_include_directories(native_lib PUBLIC
        entry/src/main/cpp
        entry/src/main/cpp/include
    )

    target_link_libraries(native_lib PUBLIC
        ai_engine           # 跨平台核心库
        ace_napi.z          # NAPI 库
        hilog_ndk.z         # OHOS 日志库
        native_media_codecbase
        native_media_vdec
        native_media_core
        dl
        m
    )

    # 链接 sherpa-onnx
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/entry/src/main/cpp/libs/${OHOS_ARCH}/libsherpa-onnx-c-api.so")
        add_library(sherpa-onnx-c-api SHARED IMPORTED)
        set_target_properties(sherpa-onnx-c-api PROPERTIES
            IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/entry/src/main/cpp/libs/${OHOS_ARCH}/libsherpa-onnx-c-api.so"
        )
        target_link_libraries(native_lib PRIVATE sherpa-onnx-c-api)
        target_compile_definitions(native_lib PRIVATE HAS_SHERPA_ONNX=1)
    else()
        message(WARNING "sherpa-onnx not found, ASR/TTS disabled")
        target_compile_definitions(native_lib PRIVATE HAS_SHERPA_ONNX=0)
    endif()

    set_target_properties(native_lib PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/libs/${OHOS_ARCH}"
        PREFIX "lib"
    )

elseif(PLATFORM STREQUAL "WINDOWS")
    # --- Windows 构建 (native_ai.dll + UI) ---
    add_subdirectory(platforms/windows)

    # Windows Qt6 Desktop UI (需要先安装 Qt6)
    # 安装方式: vcpkg install qt6-base[widgets,network] --triplet x64-windows
    add_subdirectory(ui)

    # 硬件加速 DXVA2 (可选)
    # add_subdirectory(src/hardware)

endif()

# ============================================================
# 安装规则
# ============================================================
if(PLATFORM STREQUAL "OHOS")
    install(TARGETS native_lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
    )
elseif(PLATFORM STREQUAL "WINDOWS")
    install(TARGETS native_ai ai_engine
        RUNTIME DESTINATION bin
        ARCHIVE DESTINATION lib
    )
endif()
