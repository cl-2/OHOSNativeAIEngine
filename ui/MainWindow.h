#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QLabel>
#include <QStatusBar>
#include <QTextEdit>

#include "ChatBubble.h"
class AudioButton;
class PerfDashboard;
class EngineBridge;

/**
 * @brief 主窗口
 *
 * 左侧: 聊天区 + 输入区 + 语音按钮
 * 右侧: 性能仪表盘 (可折叠)
 * 顶部: 工具栏 (设置、模型选择等)
 */
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onSendMessage();
    void onSettingsClicked();
    void onToggleAsr();
    void onToggleDashboard();
    void onEngineStateChanged(const QString& state);
    void onAsrResult(const QString& text, bool isFinal);
    void onLlmChunk(const QString& chunk);
    void onLlmResponse(const QString& response);
    void onLlmError(const QString& error);

private:
    void setupUI();
    void setupMenuBar();
    void setupStatusBar();
    void initEngine();
    void addChatBubble(const QString& text, ChatBubble::Role role);
    void updateEngineStatus();

    // UI 组件
    QWidget* m_centralWidget = nullptr;
    QVBoxLayout* m_chatLayout = nullptr;
    QScrollArea* m_chatScroll = nullptr;
    QWidget* m_chatContainer = nullptr;
    QLineEdit* m_inputEdit = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QPushButton* m_asrBtn = nullptr;
    QPushButton* m_settingsBtn = nullptr;
    QPushButton* m_dashboardBtn = nullptr;
    AudioButton* m_audioBtn = nullptr;
    PerfDashboard* m_dashboard = nullptr;
    QSplitter* m_splitter = nullptr;
    QLabel* m_engineStatusLabel = nullptr;

    // 引擎
    EngineBridge* m_engine = nullptr;
    bool m_engineReady = false;
    ChatBubble* m_currentStreamBubble = nullptr;
    QString m_streamBuffer;

    // 静态实例指针，供 C 回调使用
    static MainWindow* s_instance;
};

#endif // MAIN_WINDOW_H
