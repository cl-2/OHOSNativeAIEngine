# ============================================================
# Windows native_ai.dll 构建
# ============================================================

# 源文件
set(WINDOWS_SOURCES
    dllmain.cpp
    audio/WasapiCapture.cpp
    audio/WasapiRender.cpp
)

set(WINDOWS_HEADERS
    native_ai_api.h
    audio/WasapiCapture.h
    audio/WasapiRender.h
    audio/AudioPipeline.h
)

# 构建 DLL
add_library(native_ai SHARED
    ${WINDOWS_SOURCES}
    ${WINDOWS_HEADERS}
)

target_include_directories(native_ai PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src           # 访问 src/core, src/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/utils
)

# 链接核心引擎库
target_link_libraries(native_ai PUBLIC
    ai_engine  # 来自 src/CMakeLists.txt
)

# Windows 系统库
target_link_libraries(native_ai PUBLIC
    winmm       # Windows Multimedia API
    ole32       # COM
    oleaut32
)

# sherpa-onnx SDK (可选)
if(DEFINED SHERPA_ONNX_DIR AND EXISTS "${SHERPA_ONNX_DIR}")
    # 链接 sherpa-onnx 库
    target_link_libraries(native_ai PRIVATE
        "${SHERPA_ONNX_DIR}/lib/sherpa-onnx-c-api.lib"
    )
    target_include_directories(native_ai PUBLIC
        "${SHERPA_ONNX_DIR}/include"
    )
    target_compile_definitions(native_ai PRIVATE HAS_SHERPA_ONNX=1)

    # 构建后复制 DLL 到输出目录
    add_custom_command(TARGET native_ai POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHERPA_ONNX_DIR}/lib/sherpa-onnx-c-api.dll"
            "$<TARGET_FILE_DIR:native_ai>/sherpa-onnx-c-api.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHERPA_ONNX_DIR}/lib/onnxruntime.dll"
            "$<TARGET_FILE_DIR:native_ai>/onnxruntime.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHERPA_ONNX_DIR}/lib/onnxruntime_providers_shared.dll"
            "$<TARGET_FILE_DIR:native_ai>/onnxruntime_providers_shared.dll"
        COMMENT "Copying sherpa-onnx DLLs"
    )

    message(STATUS "sherpa-onnx enabled for Windows: ${SHERPA_ONNX_DIR}")
else()
    message(STATUS "sherpa-onnx NOT found (set -DSHERPA_ONNX_DIR=...)")
endif()

# 定义导出宏
target_compile_definitions(native_ai PRIVATE NATIVE_AI_EXPORTS)

# 安装
install(TARGETS native_ai
    RUNTIME DESTINATION bin
)
