#include "PerfDashboard.h"
#include <QGroupBox>
#include <QGridLayout>
#include <QPushButton>

PerfDashboard::PerfDashboard(QWidget* parent) : QWidget(parent) {
    setupUI();
}

void PerfDashboard::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    // 标题栏
    auto* titleBar = new QWidget(this);
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(0, 0, 0, 0);

    auto* titleLabel = new QLabel("📊 性能监控", this);
    titleLabel->setStyleSheet("font-weight:bold; font-size:14px;");

    auto* closeBtn = new QPushButton("✕", this);
    closeBtn->setFixedSize(20, 20);
    closeBtn->setStyleSheet(
        "QPushButton { background:#F44336; color:white; border:none; "
        "border-radius:10px; font-weight:bold; }"
        "QPushButton:hover { background:#D32F2F; }");
    connect(closeBtn, &QPushButton::clicked, this, &PerfDashboard::closeRequested);

    titleLayout->addWidget(titleLabel);
    titleLayout->addStretch();
    titleLayout->addWidget(closeBtn);

    mainLayout->addWidget(titleBar);

    // 指标组
    auto* group = new QGroupBox("实时指标", this);
    auto* grid = new QGridLayout(group);
    grid->setVerticalSpacing(6);

    // 行: 标签 + 数值 + 进度条
    auto addRow = [&](int row, const QString& label, QLabel*& valueLabel, QProgressBar*& bar) {
        auto* lbl = new QLabel(label, group);
        lbl->setStyleSheet("font-size:12px; color:#555;");
        valueLabel = new QLabel("--", group);
        valueLabel->setStyleSheet("font-weight:bold; font-size:12px;");
        valueLabel->setFixedWidth(80);
        bar = new QProgressBar(group);
        bar->setRange(0, 100);
        bar->setValue(0);
        bar->setTextVisible(false);
        bar->setFixedHeight(8);
        bar->setStyleSheet(
            "QProgressBar { background:#E0E0E0; border:none; border-radius:4px; }"
            "QProgressBar::chunk { background:#4CAF50; border-radius:4px; }");
        grid->addWidget(lbl, row, 0);
        grid->addWidget(valueLabel, row, 1);
        grid->addWidget(bar, row, 2);
    };

    addRow(0, "当前 RTF", m_rtfLabel, m_rtfBar);
    addRow(1, "平均 RTF", m_avgRtfLabel, m_rtfBar);  // 复用进度条显示当前
    addRow(2, "峰值 RTF", m_maxRtfLabel, m_rtfBar);

    // 延迟 (ms)
    auto* latencyLbl = new QLabel("解码延迟", group);
    latencyLbl->setStyleSheet("font-size:12px; color:#555;");
    m_latencyLabel = new QLabel("-- ms", group);
    m_latencyLabel->setStyleSheet("font-weight:bold; font-size:12px; color:#FF9800;");
    grid->addWidget(latencyLbl, 3, 0);
    grid->addWidget(m_latencyLabel, 3, 1);

    // VAD 延迟
    auto* vadLbl = new QLabel("VAD 延迟", group);
    vadLbl->setStyleSheet("font-size:12px; color:#555;");
    m_vadLabel = new QLabel("-- ms", group);
    m_vadLabel->setStyleSheet("font-weight:bold; font-size:12px; color:#2196F3;");
    grid->addWidget(vadLbl, 4, 0);
    grid->addWidget(m_vadLabel, 4, 1);

    // 降级状态
    auto* degLbl = new QLabel("降级状态", group);
    degLbl->setStyleSheet("font-size:12px; color:#555;");
    m_degradationLabel = new QLabel("正常", group);
    m_degradationLabel->setStyleSheet("font-weight:bold; font-size:12px; color:#4CAF50;");
    grid->addWidget(degLbl, 5, 0);
    grid->addWidget(m_degradationLabel, 5, 1);

    mainLayout->addWidget(group);
    mainLayout->addStretch();

    setStyleSheet("PerfDashboard { background:white; border:1px solid #DDD; border-radius:8px; }");
    setFixedWidth(320);
}

void PerfDashboard::updateMetrics(double currentRtf, double avgRtf, double maxRtf,
                                   long long latencyMs, long long vadLatencyMs, bool needsDegradation) {
    m_rtfLabel->setText(QString::number(currentRtf, 'f', 3));
    m_avgRtfLabel->setText(QString::number(avgRtf, 'f', 3));
    m_maxRtfLabel->setText(QString::number(maxRtf, 'f', 3));

    // RTF 进度条 (超过 1.0 表示实时性不足)
    int rtfPercent = qMin(static_cast<int>(currentRtf * 100.0), 100);
    m_rtfBar->setValue(rtfPercent);
    m_rtfBar->setStyleSheet(
        QString("QProgressBar { background:#E0E0E0; border:none; border-radius:4px; }"
                "QProgressBar::chunk { border-radius:4px; %1 }")
            .arg(rtfPercent > 80 ? "background:#F44336;"
                                 : rtfPercent > 50 ? "background:#FF9800;"
                                                   : "background:#4CAF50;"));

    m_latencyLabel->setText(QString::number(latencyMs) + " ms");
    m_vadLabel->setText(QString::number(vadLatencyMs) + " ms");

    if (needsDegradation) {
        m_degradationLabel->setText("⚠ 已降级");
        m_degradationLabel->setStyleSheet("font-weight:bold; font-size:12px; color:#F44336;");
    } else {
        m_degradationLabel->setText("✓ 正常");
        m_degradationLabel->setStyleSheet("font-weight:bold; font-size:12px; color:#4CAF50;");
    }
}
