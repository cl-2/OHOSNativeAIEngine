@echo off
REM ============================================================
REM Windows 原生编译脚本 (更新版)
REM 构建 ai_engine 静态库 + native_ai.dll
REM ============================================================
setlocal enabledelayedexpansion

REM 检测 Visual Studio
if "%VS170COMNTOOLS%"=="" (
    echo [ERROR] Visual Studio 2022 not found. Please run from "Developer Command Prompt for VS 2022".
    exit /b 1
)

REM 设置 vcpkg (如果存在)
if exist "%USERPROFILE%\vcpkg\scripts\buildsystems\vcpkg.cmake" (
    set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
    echo [INFO] vcpkg found at !VCPKG_ROOT!
    set "VCPKG_FLAGS=-DCMAKE_TOOLCHAIN_FILE=!VCPKG_ROOT!\scripts\buildsystems\vcpkg.cmake"
) else (
    set "VCPKG_FLAGS="
    echo [WARN] vcpkg not found. Install: git clone https://github.com/microsoft/vcpkg.git %%USERPROFILE%%\vcpkg ^&^& %%USERPROFILE%%\vcpkg\bootstrap-vcpkg.bat
)

REM 构建目录
set "BUILD_DIR=build\windows"

REM 清理
if "%1"=="clean" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo [INFO] Clean complete.
    exit /b 0
)

REM CMake 配置
echo [INFO] Configuring CMake for Windows x64...
cmake -B %BUILD_DIR% ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DPLATFORM=WINDOWS ^
    -DCMAKE_BUILD_TYPE=Release ^
    %VCPKG_FLAGS%

if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed!
    exit /b 1
)

REM 编译核心引擎 (ai_engine 静态库)
echo [INFO] Building core engine...
cmake --build %BUILD_DIR% --config Release --target ai_engine -- /maxcpucount

REM 编译 DLL
echo [INFO] Building native_ai.dll...
cmake --build %BUILD_DIR% --config Release --target native_ai -- /maxcpucount

if %ERRORLEVEL% neq 0 (
    echo [ERROR] DLL build failed!
    exit /b 1
)

echo [INFO] Build complete!
echo.
echo [INFO] Output files:
dir "%BUILD_DIR%\bin\Release\native_ai.dll" 2>nul || dir "%BUILD_DIR%\bin\native_ai.dll" 2>nul || echo   [DLL not found]
echo.
echo [INFO] Dependencies:
dumpbin /dependents "%BUILD_DIR%\bin\Release\native_ai.dll" 2>nul || echo   [dumpbin not available]

endlocal
