@echo off
REM ============================================================
REM 全平台一键构建脚本
REM 顺序构建 HarmonyOS + Windows，产出统一放置到 dist/
REM ============================================================
setlocal enabledelayedexpansion

set "DIST_DIR=dist"
set "SCRIPT_DIR=%~dp0"

echo ========================================
echo  Native AI Engine - 全平台构建
echo ========================================
echo.

REM ---- Step 1: HarmonyOS (arm64-v8a) ----
echo [1/2] Building HarmonyOS (arm64-v8a)...
call "%SCRIPT_DIR%build-harmony.bat" arm64-v8a
if %ERRORLEVEL% neq 0 (
    echo [ERROR] HarmonyOS build failed!
    exit /b 1
)
echo [OK] HarmonyOS build complete.
echo.

REM ---- Step 2: HarmonyOS (x86_64) ----
echo [2/3] Building HarmonyOS (x86_64)...
call "%SCRIPT_DIR%build-harmony.bat" x86_64
if %ERRORLEVEL% neq 0 (
    echo [WARN] HarmonyOS x86_64 build failed (non-critical for device deployment)
) else (
    echo [OK] HarmonyOS x86_64 build complete.
)
echo.

REM ---- Step 3: Windows ----
echo [3/3] Building Windows (x64)...
call "%SCRIPT_DIR%build-windows.bat"
if %ERRORLEVEL% neq 0 (
    echo [WARN] Windows build failed (requires Visual Studio 2022)
) else (
    echo [OK] Windows build complete.
)
echo.

REM ---- 收集产出 ----
echo Collecting artifacts to %DIST_DIR%/
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"

REM OHOS arm64
if exist "build\arm64-v8a\libs\arm64-v8a\libnative_lib.so" (
    mkdir "%DIST_DIR%\ohos\arm64-v8a" 2>nul
    copy "build\arm64-v8a\libs\arm64-v8a\libnative_lib.so" "%DIST_DIR%\ohos\arm64-v8a\" >nul
    echo  - libnative_lib.so (arm64-v8a)
)
REM OHOS x86_64
if exist "build\x86_64\libs\x86_64\libnative_lib.so" (
    mkdir "%DIST_DIR%\ohos\x86_64" 2>nul
    copy "build\x86_64\libs\x86_64\libnative_lib.so" "%DIST_DIR%\ohos\x86_64\" >nul
    echo  - libnative_lib.so (x86_64)
)
REM Windows
if exist "build\windows\bin\native_ai.dll" (
    mkdir "%DIST_DIR%\windows" 2>nul
    copy "build\windows\bin\native_ai.dll" "%DIST_DIR%\windows\" >nul
    echo  - native_ai.dll (x64)
)

echo.
echo ========================================
echo  Build complete! Artifacts in %DIST_DIR%/
echo ========================================

endlocal
