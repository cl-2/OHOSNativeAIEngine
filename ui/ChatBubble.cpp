#include "ChatBubble.h"
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QFrame>

ChatBubble::ChatBubble(const QString& text, Role role, QWidget* parent)
    : QWidget(parent), m_text(text), m_role(role) {
    setupUI();
}

void ChatBubble::setText(const QString& text) {
    m_text = text;
    if (m_textLabel) {
        m_textLabel->setText(text);
        updateGeometry();
    }
}

void ChatBubble::updateBubbleWidth() {
    if (!m_bubble || !parentWidget()) return;
    int maxW = qMax(parentWidget()->width() * 75 / 100, 300);
    m_bubble->setMaximumWidth(maxW);
}

void ChatBubble::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 4, 8, 4);

    // 气泡容器
    m_bubble = new QFrame(this);
    auto* bubbleLayout = new QVBoxLayout(m_bubble);
    bubbleLayout->setContentsMargins(12, 8, 12, 8);
    bubbleLayout->setSpacing(4);

    // 角色标签
    auto* roleLabel = new QLabel(m_role == User ? "你" : "AI", m_bubble);
    roleLabel->setStyleSheet(m_role == User
        ? "font-weight:bold; color:#1565C0; font-size:11px;"
        : "font-weight:bold; color:#2E7D32; font-size:11px;");

    // 气泡本身限制最大宽度 (相对于父容器)
    updateBubbleWidth();

    // 文本内容 — 自动换行
    m_textLabel = new QLabel(m_text, m_bubble);
    m_textLabel->setWordWrap(true);
    m_textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_textLabel->setStyleSheet("font-size:14px; line-height:1.5; color:#333;");

    // 时间戳
    m_timeLabel = new QLabel(QDateTime::currentDateTime().toString("HH:mm:ss"), m_bubble);
    m_timeLabel->setStyleSheet("color:#999; font-size:10px;");

    // 操作按钮行
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(4);

    m_speakBtn = new QPushButton("🔊 朗读", m_bubble);
    m_speakBtn->setFixedHeight(24);
    m_speakBtn->setStyleSheet(
        "QPushButton { background:#E3F2FD; border:1px solid #BBDEFB; "
        "border-radius:4px; padding:2px 8px; font-size:11px; }"
        "QPushButton:hover { background:#BBDEFB; }");

    m_copyBtn = new QPushButton("📋 复制", m_bubble);
    m_copyBtn->setFixedHeight(24);
    m_copyBtn->setStyleSheet(
        "QPushButton { background:#F5F5F5; border:1px solid #DDD; "
        "border-radius:4px; padding:2px 8px; font-size:11px; }"
        "QPushButton:hover { background:#E0E0E0; }");

    btnLayout->addWidget(m_speakBtn);
    btnLayout->addWidget(m_copyBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_timeLabel);

    bubbleLayout->addWidget(roleLabel);
    bubbleLayout->addWidget(m_textLabel);
    bubbleLayout->addLayout(btnLayout);

    // 气泡样式
    QString bubbleStyle = m_role == User
        ? "QFrame { background:#E3F2FD; border:1px solid #BBDEFB; border-radius:10px; }"
        : "QFrame { background:#F5F5F5; border:1px solid #E0E0E0; border-radius:10px; }";
    m_bubble->setStyleSheet(bubbleStyle);

    mainLayout->addWidget(m_bubble);

    // 对齐: 用户靠右，AI 靠左
    mainLayout->setAlignment(m_bubble, m_role == User ? Qt::AlignRight : Qt::AlignLeft);

    // 连接信号
    connect(m_speakBtn, &QPushButton::clicked, this, [this]() {
        emit speakRequested(m_text);
    });
    connect(m_copyBtn, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_text);
        m_copyBtn->setText("✅ 已复制");
    });
}
