#include "MainWindow.h"
#include "ChatBubble.h"
#include "AudioButton.h"
#include "PerfDashboard.h"
#include "SettingsPanel.h"
#include "EngineBridge.h"

#include <QMenuBar>
#include <QToolBar>
#include <QAction>
#include <QMessageBox>
#include <QSettings>
#include <QFileInfo>
#include <QApplication>
#include <QStyle>
#include <QScrollBar>

MainWindow* MainWindow::s_instance = nullptr;

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    s_instance = this;
    setWindowTitle("Native AI Engine - Windows");
    resize(1000, 700);
    setMinimumSize(700, 500);

    setupUI();
    setupMenuBar();
    setupStatusBar();
    initEngine();
}

MainWindow::~MainWindow() {
    if (m_engine && m_engine->isLoaded()) {
        m_engine->destroy();
    }
}

// ============================================================
// UI 构建
// ============================================================

void MainWindow::setupUI() {
    m_centralWidget = new QWidget(this);
    setCentralWidget(m_centralWidget);

    auto* mainLayout = new QVBoxLayout(m_centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ---- 工具栏 ----
    auto* toolbar = new QToolBar("工具栏", this);
    toolbar->setIconSize(QSize(20, 20));
    toolbar->setMovable(false);
    addToolBar(toolbar);

    m_settingsBtn = new QPushButton("⚙ 设置", this);
    m_settingsBtn->setStyleSheet("QPushButton { padding:4px 12px; border:none; }"
                                  "QPushButton:hover { background:#E0E0E0; }");
    connect(m_settingsBtn, &QPushButton::clicked, this, &MainWindow::onSettingsClicked);
    toolbar->addWidget(m_settingsBtn);

    toolbar->addSeparator();

    m_asrBtn = new QPushButton("🎙 ASR 离线", this);
    m_asrBtn->setCheckable(true);
    m_asrBtn->setStyleSheet("QPushButton { padding:4px 12px; border:none; }"
                             "QPushButton:hover { background:#E0E0E0; }"
                             "QPushButton:checked { background:#E3F2FD; color:#1565C0; font-weight:bold; }");
    connect(m_asrBtn, &QPushButton::toggled, this, &MainWindow::onToggleAsr);
    toolbar->addWidget(m_asrBtn);

    toolbar->addSeparator();

    m_dashboardBtn = new QPushButton("📊 性能", this);
    m_dashboardBtn->setCheckable(true);
    m_dashboardBtn->setStyleSheet("QPushButton { padding:4px 12px; border:none; }"
                                   "QPushButton:hover { background:#E0E0E0; }"
                                   "QPushButton:checked { background:#FFF3E0; color:#E65100; }");
    connect(m_dashboardBtn, &QPushButton::toggled, this, &MainWindow::onToggleDashboard);
    toolbar->addWidget(m_dashboardBtn);

    toolbar->addSeparator();

    m_engineStatusLabel = new QLabel("⚪ 引擎未加载", this);
    m_engineStatusLabel->setStyleSheet("padding:4px 8px; font-size:11px; color:#999;");
    toolbar->addWidget(m_engineStatusLabel);

    // ---- 分割器: 聊天区 | 仪表盘 ----
    m_splitter = new QSplitter(Qt::Horizontal, this);
    mainLayout->addWidget(m_splitter);

    // 左侧: 聊天区域
    auto* leftPanel = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(8, 8, 8, 8);
    leftLayout->setSpacing(8);

    // 聊天消息滚动区
    m_chatScroll = new QScrollArea(this);
    m_chatScroll->setWidgetResizable(true);
    m_chatScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_chatScroll->setStyleSheet("QScrollArea { border:none; background:#FAFAFA; }");

    m_chatContainer = new QWidget(this);
    m_chatLayout = new QVBoxLayout(m_chatContainer);
    m_chatLayout->setAlignment(Qt::AlignTop);
    m_chatLayout->setSpacing(4);
    m_chatLayout->addStretch();

    m_chatScroll->setWidget(m_chatContainer);
    leftLayout->addWidget(m_chatScroll, 1);

    // 输入区域
    auto* inputWidget = new QWidget(this);
    auto* inputLayout = new QHBoxLayout(inputWidget);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(6);

    m_audioBtn = new AudioButton(this);
    connect(m_audioBtn, &AudioButton::recordingStarted, this, [this]() {
        if (m_engine && m_engine->isLoaded()) {
            m_engine->startCapture(16000);
            m_engine->startASR();
            statusBar()->showMessage("录音中... 点击停止");
        }
    });
    connect(m_audioBtn, &AudioButton::recordingFinished, this, [this](const QByteArray&, int) {
        if (m_engine && m_engine->isLoaded()) {
            m_engine->stopASR();
            m_engine->stopCapture();
            m_audioBtn->setState(AudioButton::Idle);
            statusBar()->showMessage("识别完成");
        }
    });
    inputLayout->addWidget(m_audioBtn);

    m_inputEdit = new QLineEdit(this);
    m_inputEdit->setPlaceholderText("输入消息... (Ctrl+Enter 发送)");
    m_inputEdit->setStyleSheet(
        "QLineEdit { padding:10px 14px; border:2px solid #E0E0E0; "
        "border-radius:20px; font-size:14px; background:white; }"
        "QLineEdit:focus { border-color:#4CAF50; }");
    connect(m_inputEdit, &QLineEdit::returnPressed, this, &MainWindow::onSendMessage);
    inputLayout->addWidget(m_inputEdit, 1);

    m_sendBtn = new QPushButton("发送", this);
    m_sendBtn->setFixedSize(70, 40);
    m_sendBtn->setStyleSheet(
        "QPushButton { background:#4CAF50; color:white; border:none; "
        "border-radius:20px; font-size:14px; font-weight:bold; }"
        "QPushButton:hover { background:#43A047; }"
        "QPushButton:disabled { background:#BDBDBD; }");
    connect(m_sendBtn, &QPushButton::clicked, this, &MainWindow::onSendMessage);
    inputLayout->addWidget(m_sendBtn);

    leftLayout->addWidget(inputWidget);

    m_splitter->addWidget(leftPanel);

    // 右侧: 性能仪表盘
    m_dashboard = new PerfDashboard(this);
    m_dashboard->hide();
    connect(m_dashboard, &PerfDashboard::closeRequested, this, [this]() {
        m_dashboardBtn->setChecked(false);
        m_dashboard->hide();
    });
    m_splitter->addWidget(m_dashboard);

    m_splitter->setStretchFactor(0, 4);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({750, 250});
}

void MainWindow::setupMenuBar() {
    auto* fileMenu = menuBar()->addMenu("文件(&F)");
    auto* settingsAction = fileMenu->addAction("设置(&S)...");
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onSettingsClicked);
    fileMenu->addSeparator();
    auto* exitAction = fileMenu->addAction("退出(&X)");
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    auto* engineMenu = menuBar()->addMenu("引擎(&E)");
    auto* initAction = engineMenu->addAction("初始化引擎");
    connect(initAction, &QAction::triggered, this, &MainWindow::initEngine);
    auto* loadDllAction = engineMenu->addAction("手动加载 DLL...");
    connect(loadDllAction, &QAction::triggered, this, [this]() {
        // TODO: 文件选择对话框选择 native_ai.dll
    });

    auto* viewMenu = menuBar()->addMenu("视图(&V)");
    auto* dashAction = viewMenu->addAction("性能仪表盘");
    dashAction->setCheckable(true);
    connect(dashAction, &QAction::toggled, this, &MainWindow::onToggleDashboard);
}

void MainWindow::setupStatusBar() {
    statusBar()->showMessage("就绪");
    statusBar()->setStyleSheet("QStatusBar { background:#F5F5F5; border-top:1px solid #E0E0E0; }");
}

// ============================================================
// 引擎初始化
// ============================================================

void MainWindow::initEngine() {
    m_engine = &EngineBridge::instance();

    // 从设置读取模型路径
    QSettings s("NativeAI", "Engine");
    QString modelsDir = s.value("general/modelsDir", "./models").toString();

    if (m_engine->load()) {
        m_engine->init(modelsDir.toUtf8().constData());
        m_engineReady = true;
        m_engineStatusLabel->setText("🟢 引擎就绪: " + QString(m_engine->getVersion()));
        m_engineStatusLabel->setStyleSheet("padding:4px 8px; font-size:11px; color:#4CAF50;");
        statusBar()->showMessage("引擎加载成功", 3000);

        // 设置 ASR 回调 — 立即复制字符串，防止跨线程悬空指针
        m_engine->setAsrCallback(+[](const char* text, bool isFinal) {
            auto* mw = MainWindow::s_instance;
            if (mw) {
                QString safeText = QString::fromUtf8(text);
                bool safeFinal = isFinal;
                QMetaObject::invokeMethod(mw, [mw, safeText, safeFinal]() {
                    mw->onAsrResult(safeText, safeFinal);
                }, Qt::QueuedConnection);
            }
        });

        // 设置 LLM 回调 — 流式响应 (立即复制字符串，防止跨线程悬空指针)
        m_engine->setLlmCallbacks(
            // OnResponse (完整响应)
            [](const char* resp) {
                auto* mw = MainWindow::s_instance;
                if (mw) {
                    QString safeText = QString::fromUtf8(resp);
                    QMetaObject::invokeMethod(mw, [mw, safeText]() {
                        mw->onLlmResponse(safeText);
                    }, Qt::QueuedConnection);
                }
            },
            // OnStream (流式块)
            [](const char* chunk) {
                auto* mw = MainWindow::s_instance;
                if (mw) {
                    QString safeText = QString::fromUtf8(chunk);
                    QMetaObject::invokeMethod(mw, [mw, safeText]() {
                        mw->onLlmChunk(safeText);
                    }, Qt::QueuedConnection);
                }
            },
            // OnError
            [](const char* err) {
                auto* mw = MainWindow::s_instance;
                if (mw) {
                    QString safeText = QString::fromUtf8(err);
                    QMetaObject::invokeMethod(mw, [mw, safeText]() {
                        mw->onLlmError(safeText);
                    }, Qt::QueuedConnection);
                }
            }
        );

        // 从设置加载 LLM 配置
        QSettings s2("NativeAI", "Engine");
        s2.beginGroup("llm");
        QString apiUrl = s2.value("apiUrl", "http://localhost:11434/v1/chat/completions").toString();
        QString apiKey = s2.value("apiKey", "").toString();
        QString modelName = s2.value("modelName", "qwen2.5:7b").toString();
        s2.endGroup();
        m_engine->setLlmConfig(apiUrl.toUtf8().constData(), apiKey.toUtf8().constData(), modelName.toUtf8().constData());
    } else {
        m_engineReady = false;
        m_engineStatusLabel->setText("🔴 DLL 未加载");
        m_engineStatusLabel->setStyleSheet("padding:4px 8px; font-size:11px; color:#F44336;");
        statusBar()->showMessage("引擎加载失败: " + m_engine->lastError(), 5000);
    }

    updateEngineStatus();
}

// ============================================================
// 槽函数
// ============================================================

void MainWindow::onSendMessage() {
    QString text = m_inputEdit->text().trimmed();
    if (text.isEmpty()) return;

    addChatBubble(text, ChatBubble::User);
    m_inputEdit->clear();

    // 发送到 LLM
    if (m_engine && m_engine->isLoaded()) {
        m_engine->sendLlmMessageStream(text.toUtf8().constData());
        statusBar()->showMessage("等待 AI 回复...");
    } else {
        addChatBubble("(引擎未加载，请先设置并初始化)", ChatBubble::Assistant);
    }
}

void MainWindow::onSettingsClicked() {
    SettingsPanel panel(this);
    if (panel.exec() == QDialog::Accepted) {
        // 应用设置
        if (m_engine && m_engine->isLoaded()) {
            m_engine->setLlmConfig(
                panel.apiUrl().toUtf8().constData(),
                panel.apiKey().toUtf8().constData(),
                panel.modelName().toUtf8().constData());
        }
        // 如果模型目录改变，重新初始化
        QSettings s("NativeAI", "Engine");
        QString modelsDir = s.value("general/modelsDir", "./models").toString();
        if (m_engine && m_engine->isLoaded()) {
            m_engine->init(modelsDir.toUtf8().constData());
        }
    }
}

void MainWindow::onToggleAsr() {
    if (!m_engine || !m_engine->isLoaded()) {
        m_asrBtn->setChecked(false);
        QMessageBox::warning(this, "提示", "引擎未加载，无法启动 ASR");
        return;
    }

    if (m_asrBtn->isChecked()) {
        m_engine->startASR();
        m_asrBtn->setText("🎙 识别中...");
        statusBar()->showMessage("ASR 已启动");
    } else {
        m_engine->stopASR();
        m_asrBtn->setText("🎙 ASR 离线");
        statusBar()->showMessage("ASR 已停止");
    }
}

void MainWindow::onToggleDashboard() {
    m_dashboard->setVisible(m_dashboardBtn->isChecked());
    if (m_dashboardBtn->isChecked()) {
        m_dashboardBtn->setText("📊 隐藏性能");
    } else {
        m_dashboardBtn->setText("📊 性能");
    }
}

void MainWindow::onEngineStateChanged(const QString& state) {
    statusBar()->showMessage("状态: " + state);
}

void MainWindow::onAsrResult(const QString& text, bool isFinal) {
    if (isFinal) {
        addChatBubble(text, ChatBubble::User);
        // 自动发送到 LLM
        if (m_engine && m_engine->isLoaded()) {
            m_engine->sendLlmMessageStream(text.toUtf8().constData());
        }
    } else {
        // 显示中间结果到状态栏
        statusBar()->showMessage("识别中: " + text, 0);
    }
}

void MainWindow::onLlmChunk(const QString& chunk) {
    m_streamBuffer += chunk;

    if (!m_currentStreamBubble) {
        // 创建新的流式气泡
        m_currentStreamBubble = new ChatBubble(m_streamBuffer, ChatBubble::Assistant, m_chatContainer);
        m_chatLayout->insertWidget(m_chatLayout->count() - 1, m_currentStreamBubble);
        // 连接朗读按钮
        connect(m_currentStreamBubble, &ChatBubble::speakRequested, this, [this](const QString& t) {
            if (m_engine && m_engine->isLoaded()) {
                QByteArray utf8 = t.toUtf8();
                bool ok = m_engine->startTTS(utf8.constData(), 1.0f);
            }
        });
    } else {
        // 更新已有气泡
        m_currentStreamBubble->setText(m_streamBuffer);
    }

    // 自动滚动到底部
    QTimer::singleShot(50, this, [this]() {
        m_chatScroll->verticalScrollBar()->setValue(
            m_chatScroll->verticalScrollBar()->maximum());
    });
}

void MainWindow::onLlmResponse(const QString& response) {
    // 完整响应到达，更新最终内容
    if (m_currentStreamBubble) {
        m_currentStreamBubble->setText(response);
        m_currentStreamBubble = nullptr;
    }
    m_streamBuffer.clear();
    statusBar()->showMessage("就绪");
}

void MainWindow::onLlmError(const QString& error) {
    if (m_currentStreamBubble) {
        m_currentStreamBubble->setText("[错误] " + error);
        m_currentStreamBubble = nullptr;
    } else {
        addChatBubble("[错误] " + error, ChatBubble::Assistant);
    }
    m_streamBuffer.clear();
    statusBar()->showMessage("LLM 错误: " + error, 5000);
}

// ============================================================
// 辅助函数
// ============================================================

void MainWindow::addChatBubble(const QString& text, ChatBubble::Role role) {
    auto* bubble = new ChatBubble(text, role, m_chatContainer);
    // 插入到 stretch 之前
    m_chatLayout->insertWidget(m_chatLayout->count() - 1, bubble);

    // 自动滚动到底部
    QTimer::singleShot(50, this, [this]() {
        m_chatScroll->verticalScrollBar()->setValue(
            m_chatScroll->verticalScrollBar()->maximum());
    });

    connect(bubble, &ChatBubble::speakRequested, this, [this](const QString& t) {
        if (m_engine && m_engine->isLoaded()) {
            m_engine->startTTS(t.toUtf8().constData(), 1.0f);
        }
    });
}

void MainWindow::updateEngineStatus() {
    if (m_engine && m_engine->isLoaded()) {
        m_sendBtn->setEnabled(true);
    } else {
        m_sendBtn->setEnabled(false);
    }
}
