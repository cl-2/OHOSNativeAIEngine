# ============================================================
# Windows MSVC CMake Toolchain
# 用于在 Windows 平台使用 MSVC 编译 native_ai 库
# ============================================================
#
# 使用方法:
#   cmake -B build/windows -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows.cmake
#
# 依赖:
#   - Visual Studio 2022+ (含 C++ 桌面开发组件)
#   - Windows SDK 10.0+
# ============================================================

# 目标操作系统
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)

# 使用 MSVC (由 Visual Studio 提供)
# 通过 -G "Visual Studio 17 2022" 或者让 CMake 自动检测

# Windows 平台特有编译宏
add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00)

# 启用 AVX2 优化 (ONNX Runtime 推荐)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /arch:AVX2 /O2 /GL /EHsc")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /arch:AVX2 /O2 /GL")

# 链接时优化
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /LTCG")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /LTCG")

message(STATUS "Windows MSVC toolchain configured")
