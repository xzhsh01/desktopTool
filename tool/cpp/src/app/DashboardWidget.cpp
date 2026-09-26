#include "app/DashboardWidget.h"
#include "app/Theme.h"
#include "connections/ConnectionManager.h"
#include "core/Logger.h"
#include "app/MainWindow.h"
#include "database/DatabaseWidget.h"
#include "redis/RedisWidget.h"
#include "rdp/RDPWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QTimer>

DashboardWidget::DashboardWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
    refresh();

    // 连接列表变化时刷新
    connect(&ConnectionManager::instance(), &ConnectionManager::connectionsChanged,
            this, &DashboardWidget::refresh);
}

void DashboardWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    // 页面标题
    auto* header = new QLabel("仪表盘");
    header->setStyleSheet(Theme::pageHeader());
    layout->addWidget(header);

    // 统计卡片行
    auto* statsLayout = new QHBoxLayout;
    statsLayout->setSpacing(12);

    auto makeCard = [this](const QString& title, const QString& color, QLabel** valueLabel) {
        auto* card = new QFrame;
        card->setStyleSheet(Theme::card());
        card->setFixedHeight(90);
        auto* l = new QVBoxLayout(card);
        l->setContentsMargins(16, 12, 16, 12);
        auto* t = new QLabel(title);
        t->setStyleSheet(Theme::mutedText() + " border: none;");
        auto* v = new QLabel("0");
        v->setStyleSheet(QString("color: %1; font-size: 28px; font-weight: 700; border: none;").arg(color));
        l->addWidget(t);
        l->addWidget(v);
        *valueLabel = v;
        return card;
    };

    statsLayout->addWidget(makeCard("总连接数", Theme::kAccent, &m_totalLabel));
    statsLayout->addWidget(makeCard("SSH", Theme::kSuccess, &m_sshLabel));
    statsLayout->addWidget(makeCard("数据库", Theme::kWarning, &m_dbLabel));
    statsLayout->addWidget(makeCard("Redis", Theme::kCatRedis, &m_redisLabel));
    statsLayout->addWidget(makeCard("RDP", Theme::kCatRdp, &m_rdpLabel));
    statsLayout->addWidget(makeCard("活跃连接", Theme::kAccent, &m_activeLabel));

    layout->addLayout(statsLayout);

    // 最近连接表格
    auto* recentLabel = new QLabel("最近连接");
    recentLabel->setStyleSheet(Theme::sectionHeader() + " margin-top: 8px;");
    layout->addWidget(recentLabel);

    m_recentTable = new QTableWidget;
    m_recentTable->setColumnCount(5);
    m_recentTable->setHorizontalHeaderLabels({"名称", "类型", "主机", "端口", "操作"});
    m_recentTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_recentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_recentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_recentTable->verticalHeader()->setVisible(false);
    m_recentTable->setStyleSheet(Theme::card("QTableWidget"));
    layout->addWidget(m_recentTable, 1);

    // 刷新按钮
    auto* refreshBtn = new QPushButton("刷新");
    connect(refreshBtn, &QPushButton::clicked, this, &DashboardWidget::refresh);
    layout->addWidget(refreshBtn, 0, Qt::AlignLeft);
}

void DashboardWidget::refresh() {
    updateStats();
    updateRecentTable();
}

void DashboardWidget::updateStats() {
    auto& cm = ConnectionManager::instance();
    const auto& conns = cm.connections();

    m_totalLabel->setText(QString::number(conns.size()));
    m_sshLabel->setText(QString::number(cm.getByType(ConnectionManager::SSH).size()));
    m_dbLabel->setText(QString::number(cm.getByType(ConnectionManager::Database).size()));
    m_redisLabel->setText(QString::number(cm.getByType(ConnectionManager::Redis).size()));
    m_rdpLabel->setText(QString::number(cm.getByType(ConnectionManager::RDP).size()));
    m_activeLabel->setText(QString::number(cm.activeCount()));
}

void DashboardWidget::updateRecentTable() {
    auto& cm = ConnectionManager::instance();
    const auto& conns = cm.connections();

    // 按更新时间倒序，取前 10 条
    QList<ConnectionManager::Connection> sorted = conns;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.updatedAt > b.updatedAt; });

    m_recentTable->setRowCount(0);
    int row = 0;
    for (const auto& conn : sorted) {
        if (row >= 10) break;
        m_recentTable->insertRow(row);

        m_recentTable->setItem(row, 0, new QTableWidgetItem(conn.name));
        m_recentTable->setItem(row, 1, new QTableWidgetItem(ConnectionManager::connTypeDisplayName(conn.type)));
        m_recentTable->setItem(row, 2, new QTableWidgetItem(conn.host));
        m_recentTable->setItem(row, 3, new QTableWidgetItem(QString::number(conn.port)));

        auto* connectBtn = new QPushButton("连接");
        connectBtn->setFixedHeight(26);
        connect(connectBtn, &QPushButton::clicked, this, [this, id = conn.id]() {
            onQuickConnect(id);
        });
        m_recentTable->setCellWidget(row, 4, connectBtn);
        row++;
    }
}

void DashboardWidget::onQuickConnect(const QString& connId) {
    auto* conn = ConnectionManager::instance().getById(connId);
    if (!conn) return;

    Logger::instance().info(QString("快速连接: %1 (%2:%3)")
        .arg(conn->name, conn->host).arg(conn->port), "dashboard");

    // 跳转到对应页面（通过父窗口链查找 MainWindow）
    QWidget* p = parentWidget();
    while (p && !qobject_cast<MainWindow*>(p)) p = p->parentWidget();
    if (auto* mw = qobject_cast<MainWindow*>(p)) {
        switch (conn->type) {
            case ConnectionManager::SSH:
                mw->openSSHTerminal(connId);
                break;
            case ConnectionManager::Database: {
                mw->navigateToPage("database");
                auto* db = mw->findChild<DatabaseWidget*>();
                if (db) db->connectTo(connId);
                break;
            }
            case ConnectionManager::Redis: {
                mw->navigateToPage("redis");
                auto* redis = mw->findChild<RedisWidget*>();
                if (redis) redis->connectTo(connId);
                break;
            }
            case ConnectionManager::RDP: {
                mw->navigateToPage("rdp");
                auto* rdp = mw->findChild<RDPWidget*>();
                if (rdp) rdp->startSession(connId);
                break;
            }
        }
    }
}
