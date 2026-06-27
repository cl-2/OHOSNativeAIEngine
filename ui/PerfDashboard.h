#ifndef PERF_DASHBOARD_H
#define PERF_DASHBOARD_H

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QTimer>

/**
 * @brief 实时性能仪表盘
 *
 * 显示 RTF (实时因子)、延迟、VAD 耗时、是否降级等指标
 * 与 ASR 引擎的 OnPerfMetrics 回调联动
 */
class PerfDashboard : public QWidget {
    Q_OBJECT
public:
    explicit PerfDashboard(QWidget* parent = nullptr);

    void updateMetrics(double currentRtf, double avgRtf, double maxRtf,
                       long long latencyMs, long long vadLatencyMs, bool needsDegradation);

signals:
    void closeRequested();

private:
    void setupUI();

    QLabel* m_rtfLabel = nullptr;
    QLabel* m_avgRtfLabel = nullptr;
    QLabel* m_maxRtfLabel = nullptr;
    QLabel* m_latencyLabel = nullptr;
    QLabel* m_vadLabel = nullptr;
    QLabel* m_degradationLabel = nullptr;
    QProgressBar* m_rtfBar = nullptr;
};

#endif // PERF_DASHBOARD_H
