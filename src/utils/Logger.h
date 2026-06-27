#ifndef LOGGER_H
#define LOGGER_H

/**
 * @brief 跨平台日志抽象
 * 
 * Windows: 使用 printf/OutputDebugString
 * HarmonyOS: 使用 hilog
 * 
 * 用法:
 *   LOG_INFO("Tag", "message %d", 42);
 *   LOG_ERROR("Tag", "something failed: %s", err);
 */

#include <cstdio>
#include <string>

// ========== 日志级别 ==========
enum LogLevel {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3
};

// ========== 平台适配 ==========
#ifdef _WIN32
// Windows: 使用 printf (spdlog 可选)

#include <windows.h>

inline void LogPrintf(LogLevel level, const char* tag, const char* fmt, ...) {
    const char* levelStr[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    // 控制台输出
    printf("[%s][%s] %s\n", levelStr[level], tag, buffer);
    
    // 调试器输出 (Visual Studio 输出窗口)
    char debugBuf[4160];
    snprintf(debugBuf, sizeof(debugBuf), "[%s][%s] %s\n", levelStr[level], tag, buffer);
    OutputDebugStringA(debugBuf);

    // 日志文件 (便于排查)
    static FILE* g_logFile = nullptr;
    if (!g_logFile) {
        g_logFile = fopen("native_ai_log.txt", "a");
    }
    if (g_logFile) {
        fprintf(g_logFile, "[%s][%s] %s\n", levelStr[level], tag, buffer);
        fflush(g_logFile);
    }
}

#define LOG_DEBUG(tag, fmt, ...) LogPrintf(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)  LogPrintf(LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)  LogPrintf(LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...) LogPrintf(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)

#else
// HarmonyOS: 使用 hilog
#include "hilog/log.h"

#define LOG_DEBUG(tag, fmt, ...) OH_LOG_DEBUG(LOG_APP, "[%{public}s] " fmt, tag, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)  OH_LOG_INFO(LOG_APP,  "[%{public}s] " fmt, tag, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)  OH_LOG_WARN(LOG_APP,  "[%{public}s] " fmt, tag, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...) OH_LOG_ERROR(LOG_APP, "[%{public}s] " fmt, tag, ##__VA_ARGS__)

#endif

// ========== 便捷宏 (省略 tag) ==========
#define LOGD(fmt, ...) LOG_DEBUG("AIEngine", fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) LOG_INFO("AIEngine",  fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_WARN("AIEngine",  fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG_ERROR("AIEngine", fmt, ##__VA_ARGS__)

#endif // LOGGER_H
