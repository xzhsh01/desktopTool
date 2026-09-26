#pragma once

#include <QWidget>

class QLabel;
class QTableWidget;
class ConnectionManager;

/**
 * DashboardWidget: 仪表盘
 * 对应原 src/views/Dashboard.vue
 * 显示连接统计、快速访问、系统信息
 */
class DashboardWidget : public QWidget {
    Q_OBJECT

public:
    explicit DashboardWidget(QWidget* parent = nullptr);

private slots:
    void refresh();
    void onQuickConnect(const QString& connId);

private:
    void setupUI();
    QWidget* createStatCard(const QString& title, const QString& color, int& counter);
    void updateStats();
    void updateRecentTable();

    QLabel* m_totalLabel = nullptr;
    QLabel* m_sshLabel = nullptr;
    QLabel* m_dbLabel = nullptr;
    QLabel* m_redisLabel = nullptr;
    QLabel* m_rdpLabel = nullptr;
    QLabel* m_activeLabel = nullptr;
    QTableWidget* m_recentTable = nullptr;
};
