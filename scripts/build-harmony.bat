@echo off
REM ============================================================
REM HarmonyOS 交叉编译脚本
REM 使用 DevEco Studio 的 OHOS Native SDK 构建 native_lib.so
REM ============================================================
setlocal enabledelayedexpansion

REM Prefer an explicit native SDK path; otherwise derive it from DevEco Studio.
if "%OHOS_NATIVE_SDK%"=="" if not "%DEVECO_SDK_HOME%"=="" (
    set "OHOS_NATIVE_SDK=%DEVECO_SDK_HOME%\default\hms\native"
)
if "%OHOS_NATIVE_SDK%"=="" (
    echo [ERROR] Set OHOS_NATIVE_SDK or DEVECO_SDK_HOME before building.
    exit /b 1
)
if not exist "%OHOS_NATIVE_SDK%\build\cmake" (
    echo [ERROR] Invalid OHOS_NATIVE_SDK: %OHOS_NATIVE_SDK%
    exit /b 1
)

REM 设置目标架构
if "%1"=="" (
    set "OHOS_ARCH=arm64-v8a"
) else (
    set "OHOS_ARCH=%1"
)
echo [INFO] Building for OHOS_ARCH=%OHOS_ARCH%

REM 构建目录
set "BUILD_DIR=build\%OHOS_ARCH%"

REM CMake 配置
echo [INFO] Configuring CMake...
cmake -B %BUILD_DIR% ^
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-harmonyos.cmake ^
    -DOHOS_ARCH=%OHOS_ARCH% ^
    -DOHOS_NATIVE_SDK=%OHOS_NATIVE_SDK% ^
    -DPLATFORM=OHOS ^
    -DCMAKE_BUILD_TYPE=Release

if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed!
    exit /b 1
)

REM 编译
echo [INFO] Building...
cmake --build %BUILD_DIR% --config Release

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed!
    exit /b 1
)

echo [INFO] Build complete! Output: %BUILD_DIR%/libs/%OHOS_ARCH%/libnative_lib.so
endlocal
