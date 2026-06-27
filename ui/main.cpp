/**
 * @brief Native AI Engine — Windows Qt UI 入口
 *
 * 加载 native_ai.dll 并启动主窗口。
 * 支持命令行参数:
 *   --models <path>  指定模型目录
 *   --dll <path>     指定 native_ai.dll 路径
 *   --no-engine      启动时不加载引擎 (仅界面)
 */

#include "MainWindow.h"
#include "EngineBridge.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QSettings>
#include <QDebug>
#include <QDir>
#include <windows.h>
#include <stdio.h>

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("crash_log.txt", "w");
    if (f) {
        fprintf(f, "Crash! Exception code: 0x%08X\n", ep->ExceptionRecord->ExceptionCode);
        fprintf(f, "Exception address: %p\n", ep->ExceptionRecord->ExceptionAddress);
        fprintf(f, "Exception flags: 0x%X\n", ep->ExceptionRecord->ExceptionFlags);
        for (DWORD i = 0; i < ep->ExceptionRecord->NumberParameters && i < 15; i++) {
            fprintf(f, "Param[%d]: 0x%llX\n", i, ep->ExceptionRecord->ExceptionInformation[i]);
        }
        fclose(f);
    }
    MessageBoxA(NULL, "Crash! Check crash_log.txt for details.", "Fatal Error", MB_OK | MB_ICONERROR);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char* argv[]) {
    SetUnhandledExceptionFilter(CrashHandler);
    // 高 DPI 支持
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt6 默认开启 HighDpiScaling
#else
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    QApplication app(argc, argv);
    app.setApplicationName("Native AI Engine");
    app.setOrganizationName("NativeAI");
    app.setApplicationVersion("1.0.0");

    // 全局样式
    app.setStyleSheet(
        "* { font-family: 'Microsoft YaHei', 'Segoe UI', sans-serif; }"
        "QToolTip { background:#333; color:white; border:none; padding:4px 8px; font-size:12px; }");

    // 命令行解析
    QCommandLineParser parser;
    parser.setApplicationDescription("Native AI Engine Windows Client");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption modelsOpt("models", "指定模型目录", "path", "./models");
    QCommandLineOption dllOpt("dll", "指定 native_ai.dll 路径", "path");
    QCommandLineOption noEngineOpt("no-engine", "启动时不加载引擎");
    parser.addOption(modelsOpt);
    parser.addOption(dllOpt);
    parser.addOption(noEngineOpt);
    parser.process(app);

    // 如果命令行指定了模型路径，存入设置
    if (parser.isSet(modelsOpt)) {
        QSettings s("NativeAI", "Engine");
        s.setValue("general/modelsDir", parser.value(modelsOpt));
    }

    // 如果命令行指定了 DLL 路径，预加载
    if (parser.isSet(dllOpt)) {
        QSettings s("NativeAI", "Engine");
        s.setValue("general/dllPath", parser.value(dllOpt));
    }

    MainWindow window;
    window.show();

    return app.exec();
}
