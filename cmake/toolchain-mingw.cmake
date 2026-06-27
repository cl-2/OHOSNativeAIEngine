# ============================================================
# MinGW-w64 CMake Toolchain
# 用于没有 Visual Studio 时使用 GCC 编译 Windows 版本
# ============================================================
#
# 前置条件:
#   1. 安装 MinGW-w64 (https://winlibs.com/) 或通过 MSYS2
#   2. 确保 g++ 和 cmake 在 PATH 中
#
# 使用方法:
#   cmake -B build/mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake -DPLATFORM=WINDOWS
#   cmake --build build/mingw
# ============================================================

# 目标操作系统
set(CMAKE_SYSTEM_NAME Windows)

# 查找 MinGW-w64
find_program(MINGW_CXX g++ PATHS "$ENV{MINGW_HOME}/bin" "$ENV{PATH}")
find_program(MINGW_CC gcc PATHS "$ENV{MINGW_HOME}/bin" "$ENV{PATH}")
find_program(MINGW_AR ar PATHS "$ENV{MINGW_HOME}/bin" "$ENV{PATH}")
find_program(MINGW_WINDMC windmc PATHS "$ENV{MINGW_HOME}/bin" "$ENV{PATH}")
find_program(MINGW_WINDRES windres PATHS "$ENV{MINGW_HOME}/bin" "$ENV{PATH}")

if(NOT MINGW_CXX)
    message(FATAL_ERROR "MinGW-w64 g++ not found. Install from https://winlibs.com/ or via MSYS2")
endif()

set(CMAKE_C_COMPILER ${MINGW_CC})
set(CMAKE_CXX_COMPILER ${MINGW_CXX})
set(CMAKE_AR ${MINGW_AR})

message(STATUS "Using MinGW: ${CMAKE_CXX_COMPILER}")

# MinGW 特有编译标志
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX")

# 链接 WinHTTP (Windows HTTP API)
set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} -lwinhttp -lole32 -loleaut32 -luuid")

message(STATUS "MinGW toolchain configured")
