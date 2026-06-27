#include "AudioButton.h"
#include <QMouseEvent>

AudioButton::AudioButton(QWidget* parent) : QPushButton(parent) {
    setFixedSize(64, 64);
    setCursor(Qt::PointingHandCursor);
    updateAppearance();

    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(300);
    connect(m_animTimer, &QTimer::timeout, this, [this]() {
        if (m_state == Recording) {
            m_animFrame++;
            int sec = m_elapsed.elapsed() / 1000;
            // 脉冲动画: 用文字显示已录制时间
            setText(QString::number(sec) + "s");
            if (sec >= 30) {
                // 超过30秒自动停止
                stopRecording();
            }
        }
    });
}

void AudioButton::setState(State s) {
    if (m_state == s) return;
    m_state = s;
    updateAppearance();
}

void AudioButton::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        switch (m_state) {
        case Idle:
            startRecording();
            break;
        case Recording:
            stopRecording();
            break;
        case Processing:
            // Processing 状态忽略点击
            break;
        }
    }
    QPushButton::mousePressEvent(e);
}

void AudioButton::startRecording() {
    m_state = Recording;
    m_elapsed.start();
    m_animFrame = 0;
    m_animTimer->start();
    setText("0s");
    updateAppearance();
    emit recordingStarted();
}

void AudioButton::stopRecording() {
    m_state = Processing;
    m_animTimer->stop();
    setText("...");
    updateAppearance();
    // 通知 MainWindow 停止录音 (实际数据已由 DLL 直接传给 ASR)
    emit recordingFinished(QByteArray(), 16000);
}

void AudioButton::updateAppearance() {
    switch (m_state) {
    case Idle:
        setText("🎤");
        setToolTip("点击开始录音");
        setStyleSheet(
            "QPushButton { background: #4CAF50; border-radius: 32px; "
            "font-size: 24px; border: 3px solid #388E3C; }"
            "QPushButton:hover { background: #43A047; }");
        break;
    case Recording:
        setToolTip("点击停止录音");
        setStyleSheet(
            "QPushButton { background: #F44336; border-radius: 32px; "
            "font-size: 14px; font-weight: bold; color: white; "
            "border: 3px solid #D32F2F; }"
            "QPushButton:hover { background: #E53935; }");
        break;
    case Processing:
        setToolTip("正在识别中...");
        setStyleSheet(
            "QPushButton { background: #FF9800; border-radius: 32px; "
            "font-size: 18px; color: white; "
            "border: 3px solid #F57C00; }");
        break;
    }
}
