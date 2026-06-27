#ifndef SETTINGS_PANEL_H
#define SETTINGS_PANEL_H

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>

/**
 * @brief 设置面板
 *
 * 配置 LLM 连接 (API URL/Key/Model)、ASR 模型路径、TTS 参数等
 * 设置自动持久化到 QSettings
 */
class SettingsPanel : public QDialog {
    Q_OBJECT
public:
    explicit SettingsPanel(QWidget* parent = nullptr);

    // LLM 配置
    QString apiUrl() const;
    QString apiKey() const;
    QString modelName() const;

    // 模型路径
    QString modelsDir() const;

signals:
    void settingsApplied();
    void modelsDirChanged(const QString& path);

private:
    void setupUI();
    void loadSettings();
    void saveSettings();

    QLineEdit* m_apiUrlEdit = nullptr;
    QLineEdit* m_apiKeyEdit = nullptr;
    QLineEdit* m_modelNameEdit = nullptr;
    QLineEdit* m_modelsDirEdit = nullptr;
    QCheckBox* m_autoStartCheck = nullptr;
    QSpinBox* m_maxTokensSpin = nullptr;
};

#endif // SETTINGS_PANEL_H
