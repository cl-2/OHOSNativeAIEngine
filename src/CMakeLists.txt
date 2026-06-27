# ============================================================
# 跨平台核心引擎库
# 包含 ASR、TTS、LLM 等平台无关实现
# ============================================================

# 核心引擎源文件
set(CORE_SOURCES
    core/HttpLlmClient.cpp
    utils/WavWriter.cpp
)

# sherpa-onnx 源文件 (需要 sherpa-onnx SDK)
# OHOS: 通过 toolchain 自动找到
# Windows: 通过 SHERPA_ONNX_DIR 指定
if(NOT WIN32)
    list(APPEND CORE_SOURCES
        core/SherpaAsrEngine.cpp
        core/SherpaTtsEngine.cpp
    )
elseif(DEFINED SHERPA_ONNX_DIR AND EXISTS "${SHERPA_ONNX_DIR}")
    list(APPEND CORE_SOURCES
        core/SherpaAsrEngine.cpp
        core/SherpaTtsEngine.cpp
    )
    # 注意: target_include/compile 在 add_library 之后设置
    set(HAS_SHERPA_ONNX TRUE)
    message(STATUS "sherpa-onnx found: ${SHERPA_ONNX_DIR}")
endif()

# 头文件 (用于 IDE 索引)
set(CORE_HEADERS
    core/AsrEngine.h
    core/TtsEngine.h
    core/LlmClient.h
    core/SherpaAsrEngine.h
    core/SherpaTtsEngine.h
    core/HttpLlmClient.h
    utils/Logger.h
    utils/WavWriter.h
)

# 创建核心库 (静态库，供平台项目链接)
add_library(ai_engine STATIC
    ${CORE_SOURCES}
    ${CORE_HEADERS}
)

target_include_directories(ai_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

target_link_libraries(ai_engine PUBLIC
    ${CMAKE_DL_LIBS}
)

# sherpa-onnx 配置 (Windows)
if(HAS_SHERPA_ONNX)
    # 头文件路径: 优先 SDK 自带, 否则用项目内的
    if(EXISTS "${SHERPA_ONNX_DIR}/include/sherpa-onnx/c-api.h")
        target_include_directories(ai_engine PUBLIC "${SHERPA_ONNX_DIR}/include")
    else()
        # 使用项目自带的 c-api.h
        target_include_directories(ai_engine PUBLIC
            "${CMAKE_SOURCE_DIR}/entry/src/main/cpp/include"
        )
    endif()
    target_compile_definitions(ai_engine PRIVATE HAS_SHERPA_ONNX=1)
    message(STATUS "sherpa-onnx configured for ai_engine")
endif()

# 平台相关链接
if(WIN32)
    target_link_libraries(ai_engine PUBLIC
        winhttp       # Windows HTTP API
        ole32         # COM
        oleaut32
    )
    target_compile_definitions(ai_engine PRIVATE _WIN32=1)
endif()

# 设置 C++ 标准
target_compile_features(ai_engine PUBLIC cxx_std_17)
