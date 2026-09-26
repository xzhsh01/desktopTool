#include "database/DatabaseWidget.h"
#include "database/DbSessionPane.h"
#include "connections/ConnectionManager.h"
#include "core/Logger.h"
#include "app/Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QPushButton>
#include <QMenu>
#include <QMessageBox>
#include <QLabel>
#include <QToolButton>

// 统一按钮样式（深色主题）
static const char* kBtnStyle =
    "QPushButton { padding: 5px 14px; border: 1px solid #555; border-radius: 4px;"
    " background: #3a3d45; color: #ddd; }"
    "QPushButton:hover { background: #45484f; }"
    "QPushButton:pressed { background: #2e3138; }"
    "QPushButton:disabled { color: #777; background: #333; border-color: #444; }";

// 数据库类型 → tab 前缀图标（按字母前缀配色，保持简洁）
static QString dbTypeIcon(const QString& dbType) {
    if (dbType == "oracle")   return QStringLiteral("🟠");
    if (dbType == "mysql")    return QStringLiteral("🐬");
    if (dbType == "postgres") return QStringLiteral("🐘");
    if (dbType == "mssql")    return QStringLiteral("🟦");
    if (dbType == "sqlite")   return QStringLiteral("📄");
    return QStringLiteral("🗄");
}

DatabaseWidget::DatabaseWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
}

void DatabaseWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // tab 容器
    m_sessionTabs = new QTabWidget;
    m_sessionTabs->setTabsClosable(true);
    m_sessionTabs->setMovable(true);
    m_sessionTabs->setDocumentMode(true);
    m_sessionTabs->setMinimumHeight(200);

    connect(m_sessionTabs, &QTabWidget::tabCloseRequested,
            this, &DatabaseWidget::onTabCloseRequested);

    // 启动时显示空状态欢迎页（参考 SSHTermWidget）
    m_sessionTabs->addTab(buildWelcomeWidget(), "欢迎");
    m_welcomeIndex = 0;

    // 右上角 "+" 按钮：始终可点，用于在已有 tab 的情况下继续新增会话
    auto* addBtn = new QToolButton(m_sessionTabs);
    addBtn->setText("＋");
    addBtn->setToolTip("新建数据库连接");
    addBtn->setCursor(Qt::PointingHandCursor);
    addBtn->setFixedSize(18, 18);
    addBtn->setStyleSheet(
        "QToolButton { border: 1px solid #666; border-radius: 3px;"
        " background: #3a3d45; color: #ddd; font-weight: 700; font-size: 11px; padding: 0; margin: 0; }"
        "QToolButton:hover { background: #4a90e2; color: #fff; border-color: #4a90e2; }"
        "QToolButton:pressed { background: #357abd; }");
    connect(addBtn, &QToolButton::clicked, this, &DatabaseWidget::onNewConnection);
    m_sessionTabs->setCornerWidget(addBtn, Qt::TopRightCorner);

    layout->addWidget(m_sessionTabs, 1);
}

QWidget* DatabaseWidget::buildWelcomeWidget() {
    auto* w = new QWidget;
    auto* lo = new QVBoxLayout(w);
    lo->setAlignment(Qt::AlignCenter);

    auto* icon = new QLabel("💾");
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet("font-size: 48px;");
    lo->addWidget(icon);

    auto* title = new QLabel("数据库客户端");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size: 18px; color: #c8c8c8; font-weight: 600;");
    lo->addWidget(title);

    auto* hint = new QLabel("暂无数据库会话\n\n点击右上「＋ 新连接」选择已配置连接\n或从「仪表盘」快速打开数据库");
    hint->setAlignment(Qt::AlignCenter);
    hint->setStyleSheet(Theme::faintText());
    lo->addWidget(hint);

    auto* btn = new QPushButton("新建数据库连接");
    btn->setStyleSheet(kBtnStyle);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedWidth(180);
    connect(btn, &QPushButton::clicked, this, &DatabaseWidget::onNewConnection);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(btn);
    btnRow->addStretch();
    lo->addLayout(btnRow);

    return w;
}

DbSessionPane* DatabaseWidget::findSession(const QString& connId) const {
    if (connId.isEmpty()) return nullptr;
    for (DbSessionPane* s : m_sessions) {
        if (s && s->currentConnId() == connId) return s;
    }
    return nullptr;
}

void DatabaseWidget::connectTo(const QString& connectionId) {
    // 已连 → 直接切到该 tab
    if (DbSessionPane* existing = findSession(connectionId)) {

        for (int i = 0; i < m_sessions.size(); ++i) {
            if (m_sessions[i] == existing) {
                m_sessionTabs->setCurrentIndex(i);
                break;
            }
        }
        return;
    }

    // 未连 → 新建 tab + 会话
    auto* conn = ConnectionManager::instance().getById(connectionId);
    if (!conn) return;

    auto* pane = new DbSessionPane(this);
    m_sessions.append(pane);

    // 信号：tab 标题/状态指示
    connect(pane, &DbSessionPane::connecting,
            this, &DatabaseWidget::onSessionConnecting);
    connect(pane, &DbSessionPane::connected,
            this, &DatabaseWidget::onSessionConnected);
    connect(pane, &DbSessionPane::disconnected,
            this, &DatabaseWidget::onSessionDisconnected);

    // 先 addTab 新会话（保持 currentIndex 始终有效），
    // 再移除欢迎页，避免 m_sessionTabs 出现 count=0 的瞬态。
    const int idx = m_sessionTabs->addTab(pane, QString("⏳ %1").arg(conn->name));
    m_sessionTabs->setCurrentIndex(idx);

    if (m_welcomeIndex >= 0 && m_welcomeIndex < m_sessionTabs->count()) {
        QWidget* w = m_sessionTabs->widget(m_welcomeIndex);
        m_sessionTabs->removeTab(m_welcomeIndex);
        w->deleteLater();
        m_welcomeIndex = -1;
    }

    pane->connectTo(*conn);
}

void DatabaseWidget::onNewConnection() {
    const auto conns = ConnectionManager::instance().getByType(ConnectionManager::Database);
    if (conns.isEmpty()) {
        QMessageBox::information(this, "提示",
            "暂无数据库连接，请先在「应用管理」中创建。");
        return;
    }

    auto* menu = new QMenu(this);
    for (const auto& c : conns) {
        const QString label = QString("%1  %2:%3/%4")
            .arg(c.name, c.host)
            .arg(c.port)
            .arg(c.database.isEmpty() ? "-" : c.database);
        menu->addAction(label, this, [this, id = c.id]() { connectTo(id); });
    }
    // 从当前光标位置弹出菜单（欢迎页按钮或外部 connectTo 调用都可用）
    menu->exec(QCursor::pos());
    menu->deleteLater();
}

void DatabaseWidget::onSessionConnecting(const QString& connId,
                                          const QString& displayName) {
    Q_UNUSED(displayName);
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions[i] && m_sessions[i]->currentConnId() == connId) {
            m_sessionTabs->setTabText(i, QString("⏳ %1 (连接中...)")
                                          .arg(m_sessions[i]->currentConnName()));
            break;
        }
    }
}

void DatabaseWidget::onSessionConnected(const QString& connId,
                                         const QString& serverVersion) {
    for (int i = 0; i < m_sessions.size(); ++i) {
        DbSessionPane* s = m_sessions[i];
        if (!s || s->currentConnId() != connId) continue;
        m_sessionTabs->setTabText(i, QString("%1 %2")
                                      .arg(dbTypeIcon(s->currentDbType()),
                                           s->currentConnName()));
        m_sessionTabs->setTabToolTip(i, QString("%1\n%2")
                                          .arg(s->currentConnName(), serverVersion));
        break;
    }
}

void DatabaseWidget::onSessionDisconnected(const QString& connId) {
    for (int i = 0; i < m_sessions.size(); ++i) {
        DbSessionPane* s = m_sessions[i];
        if (!s || s->currentConnId() != connId) continue;
        // 区分"用户主动断开"vs"连接丢失"：标题改为断开状态
        m_sessionTabs->setTabText(i, QString("○ %1 (已断开)")
                                      .arg(s->currentConnName().isEmpty()
                                               ? QStringLiteral("未命名")
                                               : s->currentConnName()));
        break;
    }
}

void DatabaseWidget::onTabCloseRequested(int index) {
    // 欢迎页不可关闭
    if (index == m_welcomeIndex) return;

    // 真实会话下标 → m_sessions 中对应下标（欢迎页之前可能已移除，所以 index 即会话下标）
    if (index < 0 || index >= m_sessions.size()) return;
    DbSessionPane* s = m_sessions[index];
    if (!s) return;

    QString msg = QString("确定关闭连接「%1」吗？\n").arg(s->currentConnName());
    if (s->isConnected()) {
        msg += "该会话当前已连接，关闭将自动断开数据库连接。";
    } else {
        msg += "该会话未连接，关闭后释放资源。";
    }

    const auto ret = QMessageBox::question(this, "关闭会话", msg,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    // 主动断开（disconnectFromUI 内部会处理事务/执行中确认）
    s->disconnectFromUI();

    m_sessionTabs->removeTab(index);
    m_sessions.removeAt(index);
    s->deleteLater();

    // 所有会话关闭后，重新显示欢迎页
    if (m_sessions.isEmpty()) {
        m_sessionTabs->addTab(buildWelcomeWidget(), "欢迎");
        m_welcomeIndex = m_sessionTabs->count() - 1;
        m_sessionTabs->setCurrentIndex(m_welcomeIndex);
    }
}
