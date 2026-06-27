# ============================================================
# Native AI Engine — Windows Qt6 Desktop UI
# ============================================================
#
# 依赖: Qt6 (Widgets, Network)
# 构建: cmake -B build/windows -DPLATFORM=WINDOWS
#
# 使用 vcpkg 安装 Qt6:
#   vcpkg install qt6-base[network,widgets]
#
# ============================================================

# 只在 Windows 平台构建 UI
if(NOT PLATFORM STREQUAL "WINDOWS")
    return()
endif()

# ---- 查找 Qt6 ----
# 如果通过 vcpkg 安装，CMake 会自动通过 vcpkg 工具链文件找到 Qt6
# 如果系统已安装 Qt6，也会自动找到
find_package(Qt6 QUIET COMPONENTS Widgets Network)
if(NOT Qt6_FOUND)
    message(WARNING "Qt6 not found. Set CMAKE_PREFIX_PATH or install via vcpkg: vcpkg install qt6-base")
    return()
endif()

message(STATUS "Qt6 found: ${Qt6_VERSION}")

# ---- UI 可执行文件 ----
set(UI_SOURCES
    main.cpp
    MainWindow.h
    MainWindow.cpp
    EngineBridge.h
    EngineBridge.cpp
    ChatBubble.h
    ChatBubble.cpp
    AudioButton.h
    AudioButton.cpp
    PerfDashboard.h
    PerfDashboard.cpp
    SettingsPanel.h
    SettingsPanel.cpp
)

add_executable(native_ai_ui WIN32 ${UI_SOURCES})

# 启用 Qt 元对象编译 (MOC) 用于 Q_OBJECT 宏
set_target_properties(native_ai_ui PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
)

target_include_directories(native_ai_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(native_ai_ui PRIVATE
    Qt6::Widgets
    Qt6::Network
)

# 设置输出目录到 build/bin
set_target_properties(native_ai_ui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 后构建步骤: 复制 native_ai.dll 到 UI 同目录
if(TARGET native_ai)
    add_custom_command(TARGET native_ai_ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:native_ai>"
            "$<TARGET_FILE_DIR:native_ai_ui>/native_ai.dll"
        COMMENT "Copying native_ai.dll to UI output directory"
    )
endif()

message(STATUS "UI target: native_ai_ui")
