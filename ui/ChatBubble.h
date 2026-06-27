#ifndef CHAT_BUBBLE_H
#define CHAT_BUBBLE_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QDateTime>


/**
 * @brief 聊天消息气泡组件
 *
 * 区分用户消息 (右对齐/蓝色) 和 AI 回复 (左对齐/灰色)
 * 支持文本选择、复制、语音播放按钮
 */
class ChatBubble : public QWidget {
    Q_OBJECT
public:
    enum Role { User, Assistant };

    explicit ChatBubble(const QString& text, Role role, QWidget* parent = nullptr);

    void setText(const QString& text);
    QString text() const { return m_text; }
    Role role() const { return m_role; }

signals:
    void speakRequested(const QString& text);

private:
    void setupUI();
    void updateBubbleWidth();

    QString m_text;
    Role m_role;
    QFrame* m_bubble = nullptr;
    QLabel* m_textLabel = nullptr;
    QLabel* m_timeLabel = nullptr;
    QPushButton* m_speakBtn = nullptr;
    QPushButton* m_copyBtn = nullptr;
};

#endif // CHAT_BUBBLE_H
