#include "SettingsPanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QSettings>
#include <QDialogButtonBox>

SettingsPanel::SettingsPanel(QWidget* parent) : QDialog(parent) {
    setWindowTitle("设置 - Native AI Engine");
    setMinimumWidth(450);
    setupUI();
    loadSettings();
}

void SettingsPanel::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);

    // ---- LLM 配置 ----
    auto* llmGroup = new QGroupBox("LLM 配置 (Ollama / OpenAI 兼容)", this);
    auto* llmForm = new QFormLayout(llmGroup);
    llmForm->setSpacing(8);

    m_apiUrlEdit = new QLineEdit("http://localhost:11434/v1/chat/completions", this);
    m_apiUrlEdit->setPlaceholderText("例如: http://localhost:11434/v1/chat/completions");
    llmForm->addRow("API 地址:", m_apiUrlEdit);

    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setPlaceholderText("可选, Ollama 可留空");
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    llmForm->addRow("API Key:", m_apiKeyEdit);

    m_modelNameEdit = new QLineEdit("qwen2.5:7b", this);
    m_modelNameEdit->setPlaceholderText("例如: qwen2.5:7b, gpt-4o-mini");
    llmForm->addRow("模型名称:", m_modelNameEdit);

    mainLayout->addWidget(llmGroup);

    // ---- 模型路径 ----
    auto* pathGroup = new QGroupBox("模型路径", this);
    auto* pathLayout = new QHBoxLayout(pathGroup);

    m_modelsDirEdit = new QLineEdit("./models", this);
    m_modelsDirEdit->setPlaceholderText("ASR/TTS 模型目录路径");

    auto* browseBtn = new QPushButton("浏览...", this);
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, "选择模型目录", m_modelsDirEdit->text());
        if (!dir.isEmpty()) {
            m_modelsDirEdit->setText(dir);
        }
    });

    pathLayout->addWidget(m_modelsDirEdit);
    pathLayout->addWidget(browseBtn);
    mainLayout->addWidget(pathGroup);

    // ---- 其他选项 ----
    auto* miscGroup = new QGroupBox("其他", this);
    auto* miscLayout = new QVBoxLayout(miscGroup);

    m_autoStartCheck = new QCheckBox("启动时自动初始化引擎", this);
    m_autoStartCheck->setChecked(true);
    miscLayout->addWidget(m_autoStartCheck);

    // max tokens
    auto* tokensLayout = new QHBoxLayout();
    tokensLayout->addWidget(new QLabel("最大 Token 数:", this));
    m_maxTokensSpin = new QSpinBox(this);
    m_maxTokensSpin->setRange(64, 32768);
    m_maxTokensSpin->setValue(2048);
    m_maxTokensSpin->setSingleStep(128);
    tokensLayout->addWidget(m_maxTokensSpin);
    tokensLayout->addStretch();
    miscLayout->addLayout(tokensLayout);

    mainLayout->addWidget(miscGroup);

    // ---- 按钮 ----
    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btnBox, &QDialogButtonBox::accepted, this, [this]() {
        saveSettings();
        emit settingsApplied();
        accept();
    });
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(btnBox);
}

void SettingsPanel::loadSettings() {
    QSettings s("NativeAI", "Engine");
    s.beginGroup("llm");
    m_apiUrlEdit->setText(s.value("apiUrl", "http://localhost:11434/v1/chat/completions").toString());
    m_apiKeyEdit->setText(s.value("apiKey", "").toString());
    m_modelNameEdit->setText(s.value("modelName", "qwen2.5:7b").toString());
    s.endGroup();

    s.beginGroup("general");
    m_modelsDirEdit->setText(s.value("modelsDir", "./models").toString());
    m_autoStartCheck->setChecked(s.value("autoInit", true).toBool());
    m_maxTokensSpin->setValue(s.value("maxTokens", 2048).toInt());
    s.endGroup();
}

void SettingsPanel::saveSettings() {
    QSettings s("NativeAI", "Engine");
    s.beginGroup("llm");
    s.setValue("apiUrl", m_apiUrlEdit->text());
    s.setValue("apiKey", m_apiKeyEdit->text());
    s.setValue("modelName", m_modelNameEdit->text());
    s.endGroup();

    s.beginGroup("general");
    s.setValue("modelsDir", m_modelsDirEdit->text());
    s.setValue("autoInit", m_autoStartCheck->isChecked());
    s.setValue("maxTokens", m_maxTokensSpin->value());
    s.endGroup();
}

QString SettingsPanel::apiUrl() const { return m_apiUrlEdit->text(); }
QString SettingsPanel::apiKey() const { return m_apiKeyEdit->text(); }
QString SettingsPanel::modelName() const { return m_modelNameEdit->text(); }
QString SettingsPanel::modelsDir() const { return m_modelsDirEdit->text(); }
