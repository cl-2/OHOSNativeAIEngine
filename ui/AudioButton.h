#ifndef AUDIO_BUTTON_H
#define AUDIO_BUTTON_H

#include <QPushButton>
#include <QTimer>
#include <QElapsedTimer>

/**
 * @brief 语音录制/播放控制按钮
 *
 * 单击开始录音 → 再次单击停止录音并触发 ASR 识别
 * 支持状态动画: IDLE / RECORDING / PROCESSING
 */
class AudioButton : public QPushButton {
    Q_OBJECT
public:
    enum State { Idle, Recording, Processing };

    explicit AudioButton(QWidget* parent = nullptr);

    State state() const { return m_state; }
    void setState(State s);

signals:
    void recordingStarted();
    void recordingFinished(const QByteArray& pcmData, int sampleRate);
    void recordingCanceled();

protected:
    void mousePressEvent(QMouseEvent* e) override;

private:
    void updateAppearance();
    void startRecording();
    void stopRecording();

    State m_state = Idle;
    QTimer* m_animTimer = nullptr;
    QElapsedTimer m_elapsed;
    int m_animFrame = 0;
};

#endif // AUDIO_BUTTON_H
