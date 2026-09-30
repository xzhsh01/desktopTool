#include "app/MainWindow.h"
#include "core/Settings.h"
#include "connections/ConnectionManager.h"
#include "core/Logger.h"
#include "app/DashboardWidget.h"
#include "connections/ConnectionsWidget.h"
#include "ssh/SSHTermWidget.h"
#include "ssh/FileManagerWidget.h"
#include "redis/RedisWidget.h"
#include "rdp/RDPWidget.h"
#include "database/DatabaseWidget.h"
#include "mail/MailWidget.h"
#include "mail/MailPoller.h"
#include "mail/MailStore.h"
#include "mail/MailAccountManager.h"
#include "wechat/WeChatWidget.h"
#include "wechat/WeChatAccountManager.h"
#include "document/DocumentWidget.h"
#include "document/AttachmentStore.h"
#include "app/SettingsWidget.h"
#include "app/LogsWidget.h"
#include "app/AboutWidget.h"

#include <QApplication>
#include <QMenu>
#include <QCursor>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>
#include <QStyle>
#include <QMenu>
#include <QAction>
#include <QFile>
#include <QCloseEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QStyleFactory>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowFlags(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    resize(1280, 800);
    setMinimumSize(900, 600);
    setWindowTitle("bambooRat (BR)");

    // 初始化附件/文档存储
    AttachmentStore::instance().init();

    setupUI();
    setupTray();
    applyTheme();

    // 连接设置变化
    connect(&Settings::instance(), &Settings::changed, this, &MainWindow::onSettingsChanged);
    connect(&ConnectionManager::instance(), &ConnectionManager::activeChanged, this, &MainWindow::updateStatusBar);
    connect(&ConnectionManager::instance(), &ConnectionManager::connectionsChanged, this, &MainWindow::refreshSidebar);

    updateStatusBar();
    refreshSidebar();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUI() {
    auto* central = new QWidget;
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    setupTitleBar();

    // Body: sidebar + content
    auto* body = new QWidget;
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    setupSidebar();
    bodyLayout->addWidget(m_sidebar);

    m_contentStack = new QStackedWidget;
    bodyLayout->addWidget(m_contentStack, 1);

    setupPages();

    mainLayout->addWidget(m_titleBar);
    mainLayout->addWidget(body, 1);

    setupStatusBar();

    setCentralWidget(central);
}

void MainWindow::setupTitleBar() {
    m_titleBar = new QWidget;
    m_titleBar->setFixedHeight(36);
    m_titleBar->setObjectName("titleBar");

    auto* layout = new QHBoxLayout(m_titleBar);
    layout->setContentsMargins(8, 0, 0, 0);
    layout->setSpacing(0);

    // App icon + name
    m_appLabel = new QLabel("  bambooRat (BR)");
    m_appLabel->setStyleSheet("color: #c8c8c8; font-size: 13px; font-weight: 600;");

    layout->addWidget(m_appLabel);
    layout->addSpacing(12);

    // 新建按钮：走 ConnectionsWidget 的完整新建流程（含邮箱在内的所有类型），
    // 与「应用管理」页里的「新建连接」按钮完全等价
    auto* newBtn = new QToolButton;
    newBtn->setText("＋ 新建");
    newBtn->setFixedHeight(26);
    newBtn->setObjectName("titleSysBtn");
    newBtn->setCursor(Qt::PointingHandCursor);
    connect(newBtn, &QToolButton::clicked, this, [this]() {
        m_connections->showAddDialog();
    });
    layout->addWidget(newBtn);

    layout->addStretch();

    // 系统导航（设置/日志/关于）显示在顶部
    const QList<QPair<QString, QString>> sysNav = {
        {"settings", "⚙ 设置"},
        {"logs",     "📝 日志"},
        {"about",    "ℹ 关于"},
    };
    for (const auto& [id, text] : sysNav) {
        auto* btn = new QToolButton;
        btn->setText(text);
        btn->setFixedHeight(26);
        btn->setObjectName("titleSysBtn");
        btn->setProperty("pageId", id);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QToolButton::clicked, this, [this, id]() { navigateTo(id); });
        m_navButtons.append(btn);
        layout->addWidget(btn);
        layout->addSpacing(4);
    }
    layout->addSpacing(8);

    // Window control buttons
    auto* minBtn = new QToolButton;
    minBtn->setText("–");  // en-dash for minimize
    minBtn->setFixedSize(36, 36);
    minBtn->setObjectName("titleBtn");
    connect(minBtn, &QToolButton::clicked, this, &MainWindow::onMinimize);

    auto* maxBtn = new QToolButton;
    maxBtn->setText("□");  // white square
    maxBtn->setFixedSize(36, 36);
    maxBtn->setObjectName("titleBtn");
    connect(maxBtn, &QToolButton::clicked, this, &MainWindow::onMaximize);

    auto* closeBtn = new QToolButton;
    closeBtn->setText("×");  // multiplication sign
    closeBtn->setFixedSize(36, 36);
    closeBtn->setObjectName("closeBtn");
    connect(closeBtn, &QToolButton::clicked, this, &MainWindow::onClose);

    layout->addWidget(minBtn);
    layout->addWidget(maxBtn);
    layout->addWidget(closeBtn);
}

void MainWindow::setupSidebar() {
    m_sidebar = new QWidget;
    m_sidebar->setFixedWidth(162);
    m_sidebar->setObjectName("sidebar");

    auto* layout = new QVBoxLayout(m_sidebar);
    layout->setContentsMargins(4, 8, 4, 8);
    layout->setSpacing(2);

    // 功能 section
    m_funcLabel = new QLabel("  功能");
    m_funcLabel->setStyleSheet("color: #666; font-size: 11px; padding: 4px 8px;");

    struct NavItem { QString id; QString label; QString icon; };
    QList<NavItem> navItems = {
        {"dashboard",    "仪表盘",     "📊"},
        {"connections",  "应用管理",   "🔗"},
        {"ssh",          "SSH 终端",   "🖥"},
        {"database",     "数据库",     "💾"},
        {"redis",        "Redis",      "🔴"},
        {"rdp",          "远程桌面",   "💻"},
        {"mail",         "邮箱",       "📧"},
        {"wechat",       "微信",       "💬"},
        {"files",        "文件管理",   "📁"},
        {"documents",    "文档",       "📎"},
    };

    layout->addWidget(m_funcLabel);
    for (const auto& item : navItems) {
        // 用 QPushButton：QToolButton 在 ToolButtonTextOnly 下忽略 QSS text-align，
        // 导致导航文字居中；QPushButton 可靠支持 text-align: left
        auto* btn = new QPushButton;
        btn->setText(item.icon + "  " + item.label);
        // 收缩态切换用：仅图标 / 图标+文字
        btn->setProperty("iconText", item.icon);
        btn->setProperty("fullText", item.icon + "  " + item.label);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setFixedHeight(34);
        btn->setObjectName("navBtn");
        btn->setProperty("pageId", item.id);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, [this, id = item.id]() { navigateTo(id); });
        m_navButtons.append(btn);
        layout->addWidget(btn);
    }

    layout->addSpacing(8);

    // 系统导航（设置/日志/关于）已移至顶部标题栏

    layout->addStretch();

    // Collapse button
    m_collapseBtn = new QPushButton;
    m_collapseBtn->setText("«  收起侧栏");
    m_collapseBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_collapseBtn->setFixedHeight(30);
    m_collapseBtn->setObjectName("navBtn");
    m_collapseBtn->setCursor(Qt::PointingHandCursor);
    connect(m_collapseBtn, &QPushButton::clicked, [this]() {
        setSidebarCollapsed(!m_sidebarCollapsed);
    });
    layout->addWidget(m_collapseBtn);
}

void MainWindow::setSidebarCollapsed(bool collapsed) {
    m_sidebarCollapsed = collapsed;
    m_sidebar->setProperty("collapsed", collapsed);
    m_sidebar->setFixedWidth(collapsed ? 48 : 162);

    // 导航按钮收缩后仅显示图标（顶部系统导航无 iconText/fullText 属性，跳过）
    for (auto* btn : m_navButtons) {
        const QString full = btn->property("fullText").toString();
        if (full.isEmpty()) continue;
        btn->setText(collapsed ? btn->property("iconText").toString() : full);
    }
    m_funcLabel->setVisible(!collapsed);
    m_collapseBtn->setText(collapsed ? "»" : "«  收起侧栏");

    // 重新应用样式使 [collapsed] 属性选择器生效（图标居中）
    applyTheme();
}

void MainWindow::setupPages() {
    m_dashboard = new DashboardWidget;
    m_connections = new ConnectionsWidget;
    m_sshTerm = new SSHTermWidget;
    m_fileManager = new FileManagerWidget;
    m_redis = new RedisWidget;
    m_rdp = new RDPWidget;
    m_database = new DatabaseWidget;
    m_mail = new MailWidget;
    m_wechat = new WeChatWidget;
    m_documents = new DocumentWidget;
    m_settings = new SettingsWidget;
    m_logs = new LogsWidget;
    m_about = new AboutWidget;

    m_contentStack->addWidget(m_dashboard);
    m_contentStack->addWidget(m_connections);
    m_contentStack->addWidget(m_sshTerm);
    m_contentStack->addWidget(m_database);
    m_contentStack->addWidget(m_redis);
    m_contentStack->addWidget(m_rdp);
    m_contentStack->addWidget(m_mail);
    m_contentStack->addWidget(m_wechat);
    m_contentStack->addWidget(m_fileManager);
    m_contentStack->addWidget(m_documents);
    m_contentStack->addWidget(m_settings);
    m_contentStack->addWidget(m_logs);
    m_contentStack->addWidget(m_about);

    // 默认进入微信页（侧边栏调试用），通过 --start-page= 覆盖；不指定则微信
    const QStringList args = QCoreApplication::arguments();
    QString startPage = "wechat";
    for (const auto& a : args) {
        if (a.startsWith("--start-page=")) {
            startPage = a.section('=', 1).trimmed();
            break;
        }
    }
    navigateTo(startPage);
}

void MainWindow::setupStatusBar() {
    m_statusConnections = new QLabel;
    m_statusVersion = new QLabel(QString("v%1").arg(QApplication::applicationVersion()));
    statusBar()->addWidget(m_statusConnections);
    statusBar()->addPermanentWidget(m_statusVersion);
    statusBar()->setFixedHeight(24);
}

void MainWindow::setupTray() {
    m_tray = new QSystemTrayIcon(this);
    m_tray->setToolTip("bambooRat (BR)");
    // Icon will be set from resources
    m_tray->setIcon(windowIcon());

    auto* menu = new QMenu(this);
    auto* showAction = new QAction("显示主窗口", this);
    auto* quitAction = new QAction("退出", this);
    menu->addAction(showAction);
    menu->addSeparator();
    menu->addAction(quitAction);

    m_tray->setContextMenu(menu);
    connect(showAction, &QAction::triggered, this, [this]() { show(); raise(); activateWindow(); });
    connect(quitAction, &QAction::triggered, this, [this]() {
        m_isQuitting = true;
        // 窗口若处于 hide() 状态（最小化到托盘），close() 不会触发 QCloseEvent；
        // 显式调用 QApplication::quit 强制走 closeEvent 清理路径并结束事件循环。
        QApplication::quit();
    });
    connect(m_tray, &QSystemTrayIcon::activated, this, &MainWindow::onTrayActivated);

    // 新邮件到达：系统托盘气泡通知
    connect(&MailPoller::instance(), &MailPoller::notify, this,
            [this](const QString& accountName, const QString& subject, const QString& from){
        if (!m_tray) return;
        QString acc = accountName.isEmpty() ? QStringLiteral("邮箱") : accountName;
        QString title = QStringLiteral("新邮件: %1").arg(acc);
        QString body  = subject.isEmpty() ? from
                  : (from.isEmpty() ? subject : QStringLiteral("%1 — %2").arg(from, subject));
        m_tray->showMessage(title, body, QSystemTrayIcon::Information, 8000);
    });

    m_tray->show();
}

void MainWindow::applyTheme() {
    QString theme = Settings::instance().theme();
    // Dark theme stylesheet
    setStyleSheet(
        "QMainWindow { background: #1a1b23; }"
        "QLabel { color: #c8c8c8; }"
        "#titleBar { background: #16171e; border-bottom: 1px solid #2a2d36; }"
        "#titleBtn { background: transparent; border: none; color: #c8c8c8; font-size: 14px; }"
        "#titleBtn:hover { background: #2a2d36; }"
        "#closeBtn { background: transparent; border: none; color: #c8c8c8; font-size: 16px; }"
        "#closeBtn:hover { background: #e81123; color: white; }"
        "#titleSysBtn { background: transparent; border: none; border-radius: 4px; color: #8a8a8a; font-size: 12px; padding: 0 10px; }"
        "#titleSysBtn:hover { background: #2a2d36; color: #c8c8c8; }"
        "#titleSysBtn:checked { background: #2a2d36; color: #4fc3f7; }"
        "#sidebar { background: #1e1f26; border-right: 1px solid #2a2d36; }"
        "#navBtn { background: transparent; border: none; color: #8a8a8a; text-align: left; padding-left: 12px; font-size: 13px; }"
        "#navBtn:hover { background: #252830; color: #c8c8c8; }"
        "#navBtn:checked { background: #2a2d36; color: #4fc3f7; border-left: 3px solid #4fc3f7; }"
        "#sidebar[collapsed=\"true\"] #navBtn { text-align: center; padding-left: 0; font-size: 15px; }"
        "QStackedWidget { background: #1a1b23; }"
        "QStatusBar { background: #16171e; border-top: 1px solid #2a2d36; color: #666; font-size: 11px; }"
        "QStatusBar QLabel { color: #666; padding: 0 8px; }"
        "QPushButton { background: #2a2d36; color: #c8c8c8; border: 1px solid #3a3d46; border-radius: 4px; padding: 6px 16px; font-size: 13px; }"
        "QPushButton:hover { background: #3a3d46; }"
        "QPushButton:pressed { background: #1e2128; }"
        "QPushButton:disabled { color: #555; }"
        "QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: #252830; color: #c8c8c8; border: 1px solid #3a3d46; border-radius: 4px; padding: 6px; font-size: 13px; }"
        "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus { border: 1px solid #4fc3f7; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: #252830; color: #c8c8c8; selection-background-color: #4fc3f7; }"
        "QTableWidget, QTableView { background: #1e2128; color: #c8c8c8; gridline-color: #2a2d36; border: 1px solid #2a2d36; }"
        "QHeaderView::section { background: #16171e; color: #888; border: none; border-right: 1px solid #2a2d36; border-bottom: 1px solid #2a2d36; padding: 6px 8px; font-size: 12px; }"
        "QScrollBar:vertical { background: transparent; width: 8px; }"
        "QScrollBar::handle:vertical { background: #3a3d46; border-radius: 4px; min-height: 30px; }"
        "QScrollBar::handle:vertical:hover { background: #4a4d56; }"
        "QScrollBar:horizontal { background: transparent; height: 8px; }"
        "QScrollBar::handle:horizontal { background: #3a3d46; border-radius: 4px; min-width: 30px; }"
        "QTabWidget::pane { border: 1px solid #2a2d36; border-radius: 4px; }"
        "QTabBar::tab { background: #1e2128; color: #888; padding: 8px 16px; border: 1px solid #2a2d36; border-bottom: none; border-top-left-radius: 4px; border-top-right-radius: 4px; }"
        "QTabBar::tab:selected { background: #1a1b23; color: #4fc3f7; }"
        "QGroupBox { border: 1px solid #2a2d36; border-radius: 6px; margin-top: 12px; color: #c8c8c8; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }"
        "QCheckBox { color: #c8c8c8; spacing: 6px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; }"
        "QCheckBox::indicator:unchecked { background: #252830; border: 1px solid #3a3d46; border-radius: 3px; }"
        "QCheckBox::indicator:unchecked:hover { border-color: #4fc3f7; }"
        "QCheckBox::indicator:checked {"
        "  background: #4fc3f7;"
        "  border: 1px solid #4fc3f7;"
        "  border-radius: 3px;"
        // 白色对勾（qrc 资源；Qt QSS 的 image:url 不支持 base64 data: URI）
        "  image: url(\":/icons/check_white.svg\");"
        "}"
        "QCheckBox::indicator:indeterminate { background: #4fc3f7; border: 1px solid #4fc3f7; border-radius: 3px; }"
        "QCheckBox::indicator:disabled { background: #1e2128; border: 1px solid #2a2d36; border-radius: 3px; }"
        "QProgressBar { background: #252830; border: 1px solid #3a3d46; border-radius: 3px; text-align: center; color: #c8c8c8; }"
        "QProgressBar::chunk { background: #4fc3f7; border-radius: 3px; }"
        "QMenu { background: #252830; color: #c8c8c8; border: 1px solid #4fc3f7;"
        " border-radius: 6px; padding: 4px; }"
        "QMenu::item { background: transparent; padding: 6px 24px; border-radius: 4px;"
        " color: #c8c8c8; }"
        "QMenu::item:selected { background: #2a3540; color: #4fc3f7; }"
        "QMenu::item:pressed { background: #3a4555; color: #4fc3f7; }"
        "QMenu::item:disabled { color: #5a5e68; background: transparent; }"
        "QMenu::separator { height: 1px; background: #3a3d46; margin: 4px 8px; }"
        "QMenu::right-arrow { image: none; width: 8px; height: 8px;"
        " background: #c8c8c8; margin-right: 6px; }"
        "QMessageBox { background: #1e2128; }"
        "QMessageBox QLabel { color: #c8c8c8; }"
        "QToolTip { background: #2a2d36; color: #c8c8c8; border: 1px solid #3a3d46; }"
    );
}

void MainWindow::navigateTo(const QString& pageId) {
    int index = 0;
    if (pageId == "dashboard") index = 0;
    else if (pageId == "connections") index = 1;
    else if (pageId == "ssh") index = 2;
    else if (pageId == "database") index = 3;
    else if (pageId == "redis") index = 4;
    else if (pageId == "rdp") index = 5;
    else if (pageId == "mail") index = 6;
    else if (pageId == "wechat") index = 7;
    else if (pageId == "files") index = 8;
    else if (pageId == "documents") index = 9;
    else if (pageId == "settings") index = 10;
    else if (pageId == "logs") index = 11;
    else if (pageId == "about") index = 12;

    m_contentStack->setCurrentIndex(index);
    m_currentPage = pageId;

    // Update nav button states
    for (auto* btn : m_navButtons) {
        btn->setChecked(btn->property("pageId").toString() == pageId);
    }
}

void MainWindow::openSSHTerminal(const QString& connectionId) {
    navigateTo("ssh");
    if (m_sshTerm) {
        m_sshTerm->openSession(connectionId);
    }
}

void MainWindow::navigateToPage(const QString& pageId) {
    navigateTo(pageId);
}

void MainWindow::onMinimize() { showMinimized(); }

void MainWindow::onMaximize() {
    if (isMaximized()) showNormal();
    else showMaximized();
}

void MainWindow::onClose() {
    if (Settings::instance().minimizeToTray() && !m_isQuitting) {
        hide();
    } else {
        close();
    }
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::DoubleClick) {
        show();
        raise();
        activateWindow();
    }
}

void MainWindow::onSettingsChanged(const QVariantMap&) {
    applyTheme();
    refreshSidebar();  // 邮箱配置变化可能影响侧边栏显示
}

void MainWindow::refreshSidebar() {
    auto& cm = ConnectionManager::instance();
    const bool hasSsh = !cm.getByType(ConnectionManager::SSH).isEmpty();
    const bool hasDb = !cm.getByType(ConnectionManager::Database).isEmpty();
    const bool hasRedis = !cm.getByType(ConnectionManager::Redis).isEmpty();
    const bool hasRdp = !cm.getByType(ConnectionManager::RDP).isEmpty();
    const bool hasMail = !MailAccountManager::instance().accounts().isEmpty();

    for (auto* btn : m_navButtons) {
        const QString id = btn->property("pageId").toString();
        bool visible = true;  // dashboard/connections/settings/logs/about 始终显示
        if (id == "ssh" || id == "files") visible = hasSsh;      // 文件管理基于 SSH(SFTP)
        else if (id == "database") visible = hasDb;
        else if (id == "redis") visible = hasRedis;
        else if (id == "rdp") visible = hasRdp;
        else if (id == "mail") visible = hasMail;
        btn->setVisible(visible);

        // 当前页对应功能被移除时回到仪表盘
        if (!visible && id == m_currentPage) {
            QMetaObject::invokeMethod(this, [this]() { navigateTo("dashboard"); }, Qt::QueuedConnection);
        }
    }
}

void MainWindow::updateStatusBar() {
    int count = ConnectionManager::instance().activeCount();
    m_statusConnections->setText(QString("%1 个活跃连接").arg(count));
}

// ── 无边框窗口拖拽 ──────────────────────────────────────────────────────────
void MainWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->pos().y() < 36) {
        m_dragging = true;
        m_dragStartPos = event->globalPosition().toPoint() - pos();
    }
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragStartPos);
    }
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent* event) {
    m_dragging = false;
    QMainWindow::mouseReleaseEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (Settings::instance().minimizeToTray() && !m_isQuitting) {
        event->ignore();
        hide();
    } else {
        // Cleanup
        Logger::instance().info("Application shutting down", "app");
        QMainWindow::closeEvent(event);
    }
}
