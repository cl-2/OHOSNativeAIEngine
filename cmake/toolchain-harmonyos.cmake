# ============================================================
# HarmonyOS Native API CMake Toolchain
# 用于在 DevEco Studio / OpenHarmony SDK 环境下交叉编译
# ============================================================
#
# 使用方法:
#   cmake -B build/harmony -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-harmonyos.cmake
#
# 依赖:
#   - OpenHarmony Native SDK (通过 DevEco Studio 安装)
#   - OHOS_ARCH 环境变量 (arm64-v8a / x86_64)
# ============================================================

# 目标操作系统
set(CMAKE_SYSTEM_NAME OHOS)
set(CMAKE_SYSTEM_VERSION 1)

# 架构检测
if(NOT DEFINED OHOS_ARCH)
    if(DEFINED ENV{OHOS_ARCH})
        set(OHOS_ARCH $ENV{OHOS_ARCH})
    else()
        set(OHOS_ARCH "arm64-v8a")
        message(STATUS "OHOS_ARCH not set, defaulting to ${OHOS_ARCH}")
    endif()
endif()

# 查找 OHOS Native SDK
if(NOT DEFINED OHOS_NATIVE_SDK)
    if(DEFINED ENV{OHOS_NATIVE_SDK})
        set(OHOS_NATIVE_SDK $ENV{OHOS_NATIVE_SDK})
    elseif(EXISTS "$ENV{HOME}/ohos-sdk")
        set(OHOS_NATIVE_SDK "$ENV{HOME}/ohos-sdk")
    elseif(EXISTS "D:/deveco/DevEco Studio/sdk/default/hms/native")
        set(OHOS_NATIVE_SDK "D:/deveco/DevEco Studio/sdk/default/hms/native")
    else()
        message(FATAL_ERROR "OHOS_NATIVE_SDK not found. Set environment variable or install OHOS NDK.")
    endif()
endif()

message(STATUS "OHOS_NATIVE_SDK: ${OHOS_NATIVE_SDK}")
message(STATUS "OHOS_ARCH: ${OHOS_ARCH}")

# 编译器路径
if(${OHOS_ARCH} STREQUAL "arm64-v8a")
    set(CMAKE_C_COMPILER "${OHOS_NATIVE_SDK}/BiSheng/bin/clang.exe")
    set(CMAKE_CXX_COMPILER "${OHOS_NATIVE_SDK}/BiSheng/bin/clang++.exe")
    set(CMAKE_AR "${OHOS_NATIVE_SDK}/BiSheng/bin/llvm-ar.exe")
    set(CMAKE_RANLIB "${OHOS_NATIVE_SDK}/BiSheng/bin/llvm-ranlib.exe")
elseif(${OHOS_ARCH} STREQUAL "x86_64")
    set(CMAKE_C_COMPILER "${OHOS_NATIVE_SDK}/BiSheng/bin/clang.exe")
    set(CMAKE_CXX_COMPILER "${OHOS_NATIVE_SDK}/BiSheng/bin/clang++.exe")
    set(CMAKE_AR "${OHOS_NATIVE_SDK}/BiSheng/bin/llvm-ar.exe")
    set(CMAKE_RANLIB "${OHOS_NATIVE_SDK}/BiSheng/bin/llvm-ranlib.exe")
else()
    message(FATAL_ERROR "Unsupported OHOS_ARCH: ${OHOS_ARCH}")
endif()

# OHOS Native API 路径
set(OHOS_NATIVE_API_PATH "${OHOS_NATIVE_SDK}/ohos/native/api")

# 头文件路径
include_directories(
    ${OHOS_NATIVE_API_PATH}/include
    ${OHOS_NATIVE_API_PATH}/include/ace/napi
)

# 库文件路径
link_directories(
    ${OHOS_NATIVE_API_PATH}/lib/${OHOS_ARCH}
)

# 编译标志
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D__OHOS__ -target ${OHOS_ARCH}-linux-ohos")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__OHOS__ -target ${OHOS_ARCH}-linux-ohos -fvisibility=hidden -fPIC")

# 链接标志
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -target ${OHOS_ARCH}-linux-ohos")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -target ${OHOS_ARCH}-linux-ohos")

# 禁止找宿主平台的库
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

message(STATUS "HarmonyOS toolchain configured: ${OHOS_ARCH}")
