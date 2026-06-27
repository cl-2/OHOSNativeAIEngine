@echo off
REM ============================================================
REM MinGW-w64 编译脚本 (无需 Visual Studio)
REM 使用 GCC 编译 native_ai.dll
REM ============================================================
setlocal enabledelayedexpansion

REM 检查 MinGW
where g++ >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [ERROR] g++ not found in PATH.
    echo.
    echo 请安装 MinGW-w64:
    echo   方式 1 - winlibs: https://winlibs.com/ (下载并解压, 把 bin/ 加入 PATH)
    echo   方式 2 - MSYS2:  winget install MSYS2.MSYS2
    echo.
    echo 验证安装: g++ --version
    exit /b 1
)

echo [INFO] Found: 
g++ --version | findstr "g++"

REM 构建目录
set "BUILD_DIR=build\mingw"

REM 清理
if "%1"=="clean" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo [INFO] Clean complete.
    exit /b 0
)

REM CMake 配置 (使用 MinGW)
echo [INFO] Configuring CMake for MinGW...
cmake -B %BUILD_DIR% ^
    -G "MinGW Makefiles" ^
    -DCMAKE_TOOLCHAIN_FILE=cmake\toolchain-mingw.cmake ^
    -DPLATFORM=WINDOWS ^
    -DCMAKE_BUILD_TYPE=Release

if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed!
    exit /b 1
)

REM 编译
echo [INFO] Building...
cmake --build %BUILD_DIR% --config Release -j%NUMBER_OF_PROCESSORS%

if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed!
    exit /b 1
)

echo [INFO] Build complete!
echo.
echo [INFO] Output:
dir "%BUILD_DIR%\*.dll" 2>nul || dir "%BUILD_DIR%\bin\*.dll" 2>nul || echo   [check build directory]

endlocal
