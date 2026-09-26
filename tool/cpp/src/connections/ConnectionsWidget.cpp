#include "connections/ConnectionsWidget.h"
#include "connections/ConnectionManager.h"
#include "database/DriverInstaller.h"
#include "core/Logger.h"
#include "app/MainWindow.h"
#include "ssh/SSHTermWidget.h"
#include "database/DatabaseWidget.h"
#include "redis/RedisWidget.h"
#include "rdp/RDPWidget.h"
#include "mail/AccountDialog.h"
#include "mail/MailAccountManager.h"
#include "mail/MailWidget.h"
#include "wechat/WeChatAccountManager.h"
#include "wechat/WeChatConfigDialog.h"
#include "wechat/WeChatWidget.h"
#include "app/Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QTextEdit>
#include <QScrollArea>
#include <QMap>
#include <QPushButton>
#include <QMessageBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QDateTime>
#include <QTabWidget>
#include <QGridLayout>
#include <QStackedWidget>
#include <QTcpSocket>
#include <QSqlDatabase>
#include <QSqlError>
#include <QUuid>
#include <QFile>
#include <QCoreApplication>
#include <QRegularExpression>

// ── ConnectionsWidget ───────────────────────────────────────────────────────

ConnectionsWidget::ConnectionsWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
    refresh();

    connect(&ConnectionManager::instance(), &ConnectionManager::connectionsChanged,
            this, &ConnectionsWidget::refresh);

    // 邮箱账号存储在独立的 MailAccountManager 中，刷新后也要出现在分类块里
    connect(&MailAccountManager::instance(), &MailAccountManager::accountsChanged,
            this, &ConnectionsWidget::refresh);

    // 微信账号存储在独立的 WeChatAccountManager 中，同样纳入分类块
    connect(&WeChatAccountManager::instance(), &WeChatAccountManager::changed,
            this, &ConnectionsWidget::refresh);
}

void ConnectionsWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    // 标题 + 操作按钮
    auto* headerLayout = new QHBoxLayout;
    auto* header = new QLabel("应用管理");
    header->setStyleSheet(Theme::pageHeader());
    headerLayout->addWidget(header);
    headerLayout->addStretch();

    // 搜索条件：关键字 + 分类筛选
    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText("搜索名称 / 主机 / 用户 / 邮箱…");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedWidth(240);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &ConnectionsWidget::refresh);

    m_typeFilter = new QComboBox;
    m_typeFilter->addItem("全部分类", "");
    m_typeFilter->addItem("SSH 终端", "ssh");
    m_typeFilter->addItem("数据库", "database");
    m_typeFilter->addItem("Redis", "redis");
    m_typeFilter->addItem("远程桌面", "rdp");
    m_typeFilter->addItem("邮箱", "mail");
    m_typeFilter->addItem("微信", "wechat");
    m_typeFilter->setFixedWidth(110);
    connect(m_typeFilter, &QComboBox::currentIndexChanged, this, &ConnectionsWidget::refresh);

    headerLayout->addWidget(m_searchEdit);
    headerLayout->addWidget(m_typeFilter);

    auto* addBtn = new QPushButton("新建连接");
    addBtn->setCursor(Qt::PointingHandCursor);
    connect(addBtn, &QPushButton::clicked, this, &ConnectionsWidget::showAddDialog);
    headerLayout->addWidget(addBtn);

    layout->addLayout(headerLayout);

    // 分类块滚动区（refresh 时整体重建）
    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet(
        "QScrollArea { background: transparent; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }");

    m_content = new QWidget;
    m_contentLayout = new QVBoxLayout(m_content);
    m_contentLayout->setContentsMargins(0, 0, 4, 0);
    m_contentLayout->setSpacing(16);
    m_scroll->setWidget(m_content);

    layout->addWidget(m_scroll, 1);
}

void ConnectionsWidget::refresh() {
    // 清空旧的分类块
    while (m_contentLayout->count() > 0) {
        QLayoutItem* item = m_contentLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    // 搜索条件：关键字不区分大小写匹配 名称/主机/端口/用户/邮箱；
    // 分类筛选只保留对应分类块
    const QString kw = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    const QString typeSel = m_typeFilter ? m_typeFilter->currentData().toString() : QString();

    // 按分类归组连接 id："ssh"/"database"/"redis"/"rdp"/"mail"
    QMap<QString, QStringList> byKind;
    const auto& conns = ConnectionManager::instance().connections();
    for (const auto& conn : conns) {
        if (!kw.isEmpty()) {
            const QString hay = QString("%1 %2 %3 %4")
                                    .arg(conn.name, conn.host, conn.username).arg(conn.port);
            if (!hay.contains(kw, Qt::CaseInsensitive)) continue;
        }
        QString key;
        switch (conn.type) {
            case ConnectionManager::SSH:      key = "ssh"; break;
            case ConnectionManager::Database: key = "database"; break;
            case ConnectionManager::Redis:    key = "redis"; break;
            case ConnectionManager::RDP:      key = "rdp"; break;
        }
        byKind[key] << conn.id;
    }
    // 邮箱账号（由 MailAccountManager 独立管理）
    const auto& accounts = MailAccountManager::instance().accounts();
    for (const auto& a : accounts) {
        if (!kw.isEmpty()) {
            const QString hay = QString("%1 %2 %3 %4")
                                    .arg(a.name, a.email, a.imapHost, a.pop3Host);
            if (!hay.contains(kw, Qt::CaseInsensitive)) continue;
        }
        byKind["mail"] << a.id;
    }
    // 微信账号（由 WeChatAccountManager 独立管理，支持本机多账号）
    for (const auto& w : WeChatAccountManager::instance().accounts()) {
        if (!kw.isEmpty()) {
            const QString hay = QString("%1 %2 %3").arg(w.name, w.wxid, w.dataDir);
            if (!hay.contains(kw, Qt::CaseInsensitive)) continue;
        }
        byKind["wechat"] << w.id;
    }

    int total = 0;
    auto addSection = [&](const QString& type, const QString& title,
                          const QString& icon, const QString& color) {
        if (!typeSel.isEmpty() && type != typeSel) return;
        const QStringList ids = byKind.value(type);
        if (ids.isEmpty()) return;
        total += ids.size();
        if (auto* sec = makeCategorySection(type, title, icon, color, ids))
            m_contentLayout->addWidget(sec);
    };
    addSection("ssh",      "SSH 终端", "🖥", Theme::kSuccess);
    addSection("database", "数据库",   "💾", Theme::kWarning);
    addSection("redis",    "Redis",    "🔴", Theme::kCatRedis);
    addSection("rdp",      "远程桌面", "💻", Theme::kCatRdp);
    addSection("mail",     "邮箱",     "📧", Theme::kInfo);
    addSection("wechat",   "微信",     "💬", Theme::kCatWeChat);

    // 空状态提示（区分「无配置」与「无匹配」）
    if (total == 0) {
        const bool filtered = !kw.isEmpty() || !typeSel.isEmpty();
        auto* empty = new QLabel(filtered ? "未找到匹配的配置"
                                          : "暂无配置 — 点击右上角「新建连接」添加");
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(Theme::faintText() + " padding: 48px 0;");
        m_contentLayout->addWidget(empty);
    }
    m_contentLayout->addStretch();
}

QFrame* ConnectionsWidget::makeCategorySection(const QString& type, const QString& title,
                                               const QString& icon, const QString& color,
                                               const QStringList& cardIds) {
    auto* section = new QFrame;
    section->setStyleSheet(Theme::card());
    auto* v = new QVBoxLayout(section);
    v->setContentsMargins(16, 12, 16, 14);
    v->setSpacing(10);

    // 分类标题行：图标 + 名称 + 数量 + 分类内新建按钮
    auto* headRow = new QHBoxLayout;
    auto* titleLbl = new QLabel(QString("%1 %2").arg(icon, title));
    titleLbl->setStyleSheet(
        QString("color: %1; font-size: 14px; font-weight: 600; border: none;").arg(color));
    auto* countLbl = new QLabel(QString::number(cardIds.size()));
    countLbl->setStyleSheet(Theme::mutedText() + " border: none;");
    headRow->addWidget(titleLbl);
    headRow->addWidget(countLbl);
    headRow->addStretch();

    auto* addBtn = new QPushButton("＋ 新建");
    addBtn->setCursor(Qt::PointingHandCursor);
    addBtn->setStyleSheet(Theme::tableActionBtn(color));
    const QString t = type;
    connect(addBtn, &QPushButton::clicked, this, [this, t]() { addByType(t); });
    headRow->addWidget(addBtn);
    v->addLayout(headRow);

    // 卡片网格（每行 4 张，剩余空间留白）
    auto* grid = new QGridLayout;
    grid->setSpacing(10);
    int row = 0, col = 0;
    for (const QString& id : cardIds) {
        QFrame* card = nullptr;
        if (type == "mail") {
            auto* acc = MailAccountManager::instance().getById(id);
            if (!acc) continue;
            QString name = acc->name.isEmpty() ? acc->email : acc->name;
            // 显示收件服务器（IMAP 或 POP3 协议主机）
            QString recvHost = acc->recvProto == "POP3" ? acc->pop3Host : acc->imapHost;
            int recvPort = acc->recvProto == "POP3" ? acc->pop3Port : acc->imapPort;
            card = makeCard(id, "mail", name,
                            QString("%1:%2 · %3").arg(recvHost).arg(recvPort).arg(acc->email),
                            color, acc->isDefault ? "★" : QString());
        } else if (type == "wechat") {
            auto* acc = WeChatAccountManager::instance().getById(id);
            if (!acc) continue;
            card = makeCard(id, "wechat",
                            acc->name.isEmpty() ? acc->wxid : acc->name,
                            QString("%1 · %2").arg(acc->wxid, acc->version),
                            color, QString());
        } else {
            auto* conn = ConnectionManager::instance().getById(id);
            if (!conn) continue;
            card = makeCard(id, "conn", conn->name,
                            QString("%1:%2 · %3").arg(conn->host).arg(conn->port).arg(conn->username),
                            color, conn->active ? "●" : QString());
        }
        grid->addWidget(card, row, col);
        if (++col >= 4) { col = 0; ++row; }
    }
    grid->setColumnStretch(4, 1);
    v->addLayout(grid);
    return section;
}

QFrame* ConnectionsWidget::makeCard(const QString& id, const QString& kind,
                                    const QString& name, const QString& subtitle,
                                    const QString& color, const QString& badge) {
    Q_UNUSED(kind);
    auto* card = new QFrame;
    card->setObjectName("connCard");
    card->setProperty("connId", id);
    card->setStyleSheet(Theme::card("QFrame#connCard") +
        QString("QFrame#connCard:hover { border: 1px solid %1; }").arg(color));
    card->setFixedSize(250, 100);
    card->setCursor(Qt::PointingHandCursor);
    card->installEventFilter(this);

    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(12, 10, 12, 10);
    v->setSpacing(4);

    // 名称 + 状态标记（● 活跃 / ★ 默认账号）
    auto* nameLbl = new QLabel(badge.isEmpty() ? name : name + "  " + badge);
    nameLbl->setStyleSheet(QString(
        "color: %1; font-size: 13px; font-weight: 600; border: none;").arg(Theme::kTextBright));
    nameLbl->setToolTip(name);
    auto* subLbl = new QLabel(subtitle);
    subLbl->setStyleSheet(Theme::mutedText() + " border: none;");
    subLbl->setToolTip(subtitle);

    v->addWidget(nameLbl);
    v->addWidget(subLbl);
    v->addStretch();

    // 操作按钮行：连接 / 编辑 / 删除
    auto* btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);
    auto* connBtn = new QPushButton("连接");
    auto* editBtn = new QPushButton("编辑");
    auto* delBtn  = new QPushButton("删除");
    connBtn->setStyleSheet(Theme::tableActionBtn(Theme::kSuccess));
    editBtn->setStyleSheet(Theme::tableActionBtn(Theme::kInfo));
    delBtn->setStyleSheet(Theme::tableActionBtn(Theme::kDanger));
    for (auto* b : {connBtn, editBtn, delBtn}) {
        b->setFixedHeight(24);
        b->setCursor(Qt::PointingHandCursor);
    }
    btnRow->addWidget(connBtn);
    btnRow->addWidget(editBtn);
    btnRow->addStretch();
    btnRow->addWidget(delBtn);
    v->addLayout(btnRow);

    connect(connBtn, &QPushButton::clicked, this, [this, id]() { connectById(id); });
    connect(editBtn, &QPushButton::clicked, this, [this, id]() { editConnection(id); });
    connect(delBtn,  &QPushButton::clicked, this, [this, id]() { deleteConnection(id); });
    return card;
}

bool ConnectionsWidget::eventFilter(QObject* obj, QEvent* event) {
    // 双击卡片直接连接
    if (event->type() == QEvent::MouseButtonDblClick) {
        if (auto* card = qobject_cast<QFrame*>(obj)) {
            QString id = card->property("connId").toString();
            if (!id.isEmpty()) {
                connectById(id);
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void ConnectionsWidget::showAddDialog() {
    // 先弹出「选择应用」页面，让用户选择应用类型
    AppTypeSelectorDialog selector(this);
    if (selector.exec() != QDialog::Accepted) return;
    addByType(selector.selectedType(), selector.selectedDbType());
}

void ConnectionsWidget::addByType(const QString& type, const QString& dbType) {
    if (type.isEmpty()) return;

    // 邮箱账号走独立的 MailAccountManager 管理，
    // 在此直接弹 AccountDialog 添加并切换到邮箱 Tab。
    if (type == "mail") {
        AccountDialog dlg("new", {}, this);
        if (dlg.exec() != QDialog::Accepted) return;

        MailAccountManager::instance().add(dlg.result());
        Logger::instance().success("邮箱账号已添加", "mail");

        // 跳转到主窗口的「邮箱」页面，让用户立即看到新账号
        if (auto* mw = qobject_cast<MainWindow*>(window())) {
            mw->navigateToPage("mail");
        }
        return;
    }

    // 微信账号走独立的 WeChatAccountManager 管理，
    // 弹 WeChatConfigDialog（扫描本机微信 + 配置密钥），保存后跳转微信页面。
    if (type == "wechat") {
        WeChatConfigDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted) return;
        Logger::instance().success("微信账号已添加", "wechat");
        if (auto* mw = qobject_cast<MainWindow*>(window())) {
            mw->navigateToPage("wechat");
        }
        return;
    }

    // 根据类型打开连接表单并预设类型（数据库子类型在表单内选择）
    ConnectionDialog dlg(QString(), this);
    dlg.presetType(type);
    if (type == "database" && !dbType.isEmpty()) {
        dlg.presetDbType(dbType);
    }
    if (dlg.exec() == QDialog::Accepted) {
        ConnectionManager::instance().add(dlg.formData());
        Logger::instance().success("连接已创建", "connections");
    }
}

void ConnectionsWidget::editConnection(const QString& id) {
    // 邮箱账号由 MailAccountManager 管理，走 AccountDialog
    if (MailAccountManager::instance().getById(id)) {
        auto* acc = MailAccountManager::instance().getById(id);
        if (!acc) return;
        QVariantMap data;
        data["id"]            = acc->id;
        data["name"]          = acc->name;
        data["email"]         = acc->email;
        data["displayName"]   = acc->displayName;
        data["smtpHost"]      = acc->smtpHost;
        data["smtpPort"]      = acc->smtpPort;
        data["smtpSsl"]       = acc->smtpSsl;
        data["recvProto"]     = acc->recvProto;
        data["imapHost"]      = acc->imapHost;
        data["imapPort"]      = acc->imapPort;
        data["imapSsl"]       = acc->imapSsl;
        data["pop3Host"]      = acc->pop3Host;
        data["pop3Port"]      = acc->pop3Port;
        data["pop3Ssl"]       = acc->pop3Ssl;
        data["calDavEnabled"] = acc->calDavEnabled;
        data["calDavHost"]    = acc->calDavHost;
        data["calDavPort"]    = acc->calDavPort;
        data["calDavSsl"]     = acc->calDavSsl;
        data["calDavUser"]    = acc->calDavUser;
        // 密码不在 UI 中回显，保持原值
        AccountDialog dlg("edit", data, this);
        if (dlg.exec() != QDialog::Accepted) return;
        QVariantMap result = dlg.result();
        result["password"] = acc->password;
        if (MailAccountManager::instance().update(id, result)) {
            Logger::instance().success("邮箱账号已更新", "mail");
        }
        return;
    }

    // 微信账号由 WeChatAccountManager 管理，走 WeChatConfigDialog
    if (WeChatAccountManager::instance().getById(id)) {
        WeChatConfigDialog dlg(this, id);
        if (dlg.exec() == QDialog::Accepted) {
            Logger::instance().success("微信账号已更新", "wechat");
        }
        return;
    }

    ConnectionDialog dlg(id, this);
    if (dlg.exec() == QDialog::Accepted) {
        ConnectionManager::instance().update(id, dlg.formData());
        Logger::instance().success("连接已更新", "connections");
    }
}

void ConnectionsWidget::deleteConnection(const QString& id) {
    if (MailAccountManager::instance().getById(id)) {
        auto* acc = MailAccountManager::instance().getById(id);
        QString label = acc ? (acc->name.isEmpty() ? acc->email : acc->name) : id;
        auto ret = QMessageBox::question(this, "删除邮箱账号",
            QString("确定删除邮箱账号「%1」吗？").arg(label));
        if (ret == QMessageBox::Yes) {
            MailAccountManager::instance().remove(id);
            Logger::instance().info(QString("邮箱账号已删除: %1").arg(label), "mail");
        }
        return;
    }

    // 微信账号由 WeChatAccountManager 管理
    if (WeChatAccountManager::instance().getById(id)) {
        auto* acc = WeChatAccountManager::instance().getById(id);
        QString label = acc ? (acc->name.isEmpty() ? acc->wxid : acc->name) : id;
        auto ret = QMessageBox::question(this, "删除微信账号",
            QString("确定删除微信账号「%1」吗？\n（仅删除本工具中的配置与缓存，不影响微信本体数据）").arg(label));
        if (ret == QMessageBox::Yes) {
            WeChatAccountManager::instance().remove(id);
            Logger::instance().info(QString("微信账号已删除: %1").arg(label), "wechat");
        }
        return;
    }

    auto* conn = ConnectionManager::instance().getById(id);
    QString name = conn ? conn->name : id;

    auto ret = QMessageBox::question(this, "删除连接",
        QString("确定删除连接「%1」吗？").arg(name));
    if (ret == QMessageBox::Yes) {
        ConnectionManager::instance().setActive(id, false);
        ConnectionManager::instance().remove(id);
        Logger::instance().info(QString("连接已删除: %1").arg(name), "connections");
    }
}

void ConnectionsWidget::connectById(const QString& id) {
    // 邮箱账号：跳到 mail 页面并选中该账号（IMAP 数据由 MailWidget 异步加载）
    if (MailAccountManager::instance().getById(id)) {
        QWidget* p = parentWidget();
        while (p && !qobject_cast<MainWindow*>(p)) p = p->parentWidget();
        if (!p) return;
        auto* mw = qobject_cast<MainWindow*>(p);
        mw->navigateToPage("mail");
        if (auto* mail = mw->findChild<MailWidget*>()) mail->selectAccount(id);
        return;
    }

    // 微信账号：跳到微信页面并选中该账号（数据由 WeChatWidget 解密加载）
    if (WeChatAccountManager::instance().getById(id)) {
        QWidget* p = parentWidget();
        while (p && !qobject_cast<MainWindow*>(p)) p = p->parentWidget();
        if (!p) return;
        auto* mw = qobject_cast<MainWindow*>(p);
        mw->navigateToPage("wechat");
        if (auto* wx = mw->findChild<WeChatWidget*>()) wx->selectAccount(id);
        return;
    }

    auto* conn = ConnectionManager::instance().getById(id);
    if (!conn) return;

    ConnectionManager::instance().setActive(id, true);

    // 查找 MainWindow 并跳转到对应页面
    QWidget* p = parentWidget();
    while (p && !qobject_cast<MainWindow*>(p)) p = p->parentWidget();
    if (!p) return;
    auto* mw = qobject_cast<MainWindow*>(p);

    switch (conn->type) {
        case ConnectionManager::SSH:
            mw->openSSHTerminal(id);
            break;
        case ConnectionManager::Database:
            mw->navigateToPage("database");
            if (auto* db = mw->findChild<DatabaseWidget*>()) db->connectTo(id);
            break;
        case ConnectionManager::Redis:
            mw->navigateToPage("redis");
            if (auto* redis = mw->findChild<RedisWidget*>()) redis->connectTo(id);
            break;
        case ConnectionManager::RDP:
            mw->navigateToPage("rdp");
            if (auto* rdp = mw->findChild<RDPWidget*>()) rdp->startSession(id);
            break;
    }
}

// ── ConnectionDialog ────────────────────────────────────────────────────────

ConnectionDialog::ConnectionDialog(const QString& editId, QWidget* parent)
    : QDialog(parent), m_editId(editId) {
    setWindowTitle(editId.isEmpty() ? "新建连接" : "编辑连接");
    setMinimumWidth(460);
    setupUI();
    if (!editId.isEmpty()) {
        loadFromConnection(editId);
    }
    updateFieldVisibility();
}

void ConnectionDialog::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto* form = new QFormLayout;
    form->setSpacing(10);

    m_nameEdit = new QLineEdit;
    m_nameEdit->setPlaceholderText("连接名称");
    form->addRow("名称:", m_nameEdit);

    m_typeCombo = new QComboBox;
    m_typeCombo->addItem("SSH 终端", "ssh");
    m_typeCombo->addItem("数据库", "database");
    m_typeCombo->addItem("Redis", "redis");
    m_typeCombo->addItem("远程桌面", "rdp");
    connect(m_typeCombo, &QComboBox::currentIndexChanged, this, [this]() { updateFieldVisibility(); });
    form->addRow("类型:", m_typeCombo);

    m_dbTypeCombo = new QComboBox;
    m_dbTypeCombo->addItem("MySQL", "mysql");
    m_dbTypeCombo->addItem("PostgreSQL", "postgres");
    m_dbTypeCombo->addItem("SQLite", "sqlite");
    m_dbTypeCombo->addItem("SQL Server", "mssql");
    m_dbTypeCombo->addItem("Oracle", "oracle");
    connect(m_dbTypeCombo, &QComboBox::currentIndexChanged, this, [this]() { updateFieldVisibility(); });
    m_dbTypeLabel = new QLabel("数据库类型:");
    form->addRow(m_dbTypeLabel, m_dbTypeCombo);

    m_hostEdit = new QLineEdit;
    m_hostEdit->setPlaceholderText("主机地址");
    m_hostLabel = new QLabel("主机:");
    form->addRow(m_hostLabel, m_hostEdit);

    m_portSpin = new QSpinBox;
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_portLabel = new QLabel("端口:");
    form->addRow(m_portLabel, m_portSpin);

    // 数据库连接串（database 类型且非 SQLite 时显示，替代主机/端口/数据库）
    m_connStrEdit = new QLineEdit;
    m_connStrEdit->setPlaceholderText(
        "host:端口/数据库名  或完整 ODBC 连接串\n"
        "如 192.168.1.10:1521/orcl 或 Driver={...};Server=...;Database=...");
    m_connStrLabel = new QLabel("数据库连接:");
    form->addRow(m_connStrLabel, m_connStrEdit);

    m_userEdit = new QLineEdit;
    m_userEdit->setPlaceholderText("用户名");
    form->addRow("用户名:", m_userEdit);

    m_passEdit = new QLineEdit;
    m_passEdit->setEchoMode(QLineEdit::Password);
    m_passEdit->setPlaceholderText("密码");
    m_passLabel = new QLabel("密码:");
    form->addRow(m_passLabel, m_passEdit);

    {
        m_dbEdit = new QLineEdit;
        m_dbEdit->setPlaceholderText("数据库名（SQLite 为文件路径）");
        auto* browseBtn = new QPushButton("浏览...");
        browseBtn->setFixedWidth(70);
        connect(browseBtn, &QPushButton::clicked, this, &ConnectionDialog::browseSqliteFile);
        auto* wrap = new QWidget;
        auto* hl = new QHBoxLayout(wrap);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);
        hl->addWidget(m_dbEdit);
        hl->addWidget(browseBtn);
        m_dbLabel = new QLabel("数据库:");
        form->addRow(m_dbLabel, wrap);
    }

    m_authTypeCombo = new QComboBox;
    m_authTypeCombo->addItem("密码", "password");
    m_authTypeCombo->addItem("私钥", "key");
    connect(m_authTypeCombo, &QComboBox::currentIndexChanged, this, [this]() { updateFieldVisibility(); });
    m_authLabel = new QLabel("SSH 认证:");
    form->addRow(m_authLabel, m_authTypeCombo);

    {
        m_keyPathEdit = new QLineEdit;
        m_keyPathEdit->setPlaceholderText("私钥文件路径");
        auto* browseBtn = new QPushButton("浏览...");
        browseBtn->setFixedWidth(70);
        connect(browseBtn, &QPushButton::clicked, this, &ConnectionDialog::browsePrivateKey);
        auto* wrap = new QWidget;
        auto* hl = new QHBoxLayout(wrap);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);
        hl->addWidget(m_keyPathEdit);
        hl->addWidget(browseBtn);
        m_keyLabel = new QLabel("私钥:");
        form->addRow(m_keyLabel, wrap);
    }

    {
        m_instantClientEdit = new QLineEdit;
        m_instantClientEdit->setPlaceholderText("Oracle Instant Client 目录（可选）");
        auto* browseBtn = new QPushButton("浏览...");
        browseBtn->setFixedWidth(70);
        connect(browseBtn, &QPushButton::clicked, this, &ConnectionDialog::browseInstantClient);
        auto* wrap = new QWidget;
        auto* hl = new QHBoxLayout(wrap);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);
        hl->addWidget(m_instantClientEdit);
        hl->addWidget(browseBtn);
        m_clientLabel = new QLabel("Instant Client:");
        form->addRow(m_clientLabel, wrap);
    }

    m_descEdit = new QTextEdit;
    m_descEdit->setMaximumHeight(60);
    m_descEdit->setPlaceholderText("备注（可选）");
    form->addRow("描述:", m_descEdit);

    layout->addLayout(form);

    // 按钮：测试连接 + 确定/取消
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    m_testBtn = buttons->addButton("测试连接", QDialogButtonBox::ActionRole);
    m_testBtn->setCursor(Qt::PointingHandCursor);
    connect(m_testBtn, &QPushButton::clicked, this, &ConnectionDialog::testConnection);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ConnectionDialog::presetType(const QString& type) {
    int idx = m_typeCombo->findData(type);
    if (idx >= 0) m_typeCombo->setCurrentIndex(idx);
    // 从「新建」菜单进入时类型已确定，锁定避免误改
    m_typeCombo->setEnabled(false);
    updateFieldVisibility();
}

void ConnectionDialog::presetDbType(const QString& dbType) {
    int idx = m_dbTypeCombo->findData(dbType);
    if (idx >= 0) m_dbTypeCombo->setCurrentIndex(idx);
    // 类型已从选择页面确定，锁定数据库类型选择
    m_dbTypeCombo->setEnabled(false);
    updateFieldVisibility();
}

void ConnectionDialog::updateFieldVisibility() {
    QString type = m_typeCombo->currentData().toString();
    QString dbType = m_dbTypeCombo->currentData().toString();
    bool isDb = (type == "database");
    bool isSsh = (type == "ssh");
    bool isOracle = isDb && dbType == "oracle";
    bool isSqlite = isDb && dbType == "sqlite";
    bool useKey = isSsh && m_authTypeCombo->currentData().toString() == "key";

    // 数据库相关字段
    m_dbTypeLabel->setVisible(isDb);
    m_dbTypeCombo->setVisible(isDb);
    m_clientLabel->setVisible(isOracle);
    m_instantClientEdit->parentWidget()->setVisible(isOracle);

    // 数据库（非 SQLite）：使用「数据库连接」输入框，隐藏主机/端口/数据库名
    bool useConnStr = isDb && !isSqlite;
    m_connStrLabel->setVisible(useConnStr);
    m_connStrEdit->setVisible(useConnStr);
    m_hostLabel->setVisible(!useConnStr);
    m_hostEdit->setVisible(!useConnStr);
    m_portLabel->setVisible(!useConnStr);
    m_portSpin->setVisible(!useConnStr);
    m_dbLabel->setVisible(isDb && isSqlite);
    m_dbEdit->parentWidget()->setVisible(isDb && isSqlite);

    // SSH 认证字段
    m_authLabel->setVisible(isSsh);
    m_authTypeCombo->setVisible(isSsh);
    m_keyLabel->setVisible(useKey);
    m_keyPathEdit->parentWidget()->setVisible(useKey);

    // 密码：SSH 密钥模式下隐藏
    m_passLabel->setVisible(!useKey);
    m_passEdit->setVisible(!useKey);

    // SQLite：主机不需要
    m_hostEdit->setEnabled(!isSqlite);

    // 更新默认端口（仅当用户未手动修改过端口时——简化：类型切换时总是更新）
    if (type == "ssh") m_portSpin->setValue(22);
    else if (type == "redis") m_portSpin->setValue(6379);
    else if (type == "rdp") m_portSpin->setValue(3389);
    else if (isDb) m_portSpin->setValue(ConnectionManager::defaultPort(dbType));
}

void ConnectionDialog::browsePrivateKey() {
    QString path = QFileDialog::getOpenFileName(this, "选择私钥文件");
    if (!path.isEmpty()) m_keyPathEdit->setText(path);
}

void ConnectionDialog::browseSqliteFile() {
    QString path = QFileDialog::getOpenFileName(this, "选择 SQLite 文件", QString(), "SQLite (*.db *.sqlite *.sqlite3);;所有文件 (*)");
    if (!path.isEmpty()) {
        m_dbEdit->setText(path);
        m_hostEdit->setText("localhost");
        m_hostEdit->setEnabled(false);
    }
}

void ConnectionDialog::browseInstantClient() {
    QString path = QFileDialog::getExistingDirectory(this, "选择 Oracle Instant Client 目录");
    if (!path.isEmpty()) m_instantClientEdit->setText(path);
}

void ConnectionDialog::loadFromConnection(const QString& id) {
    auto* conn = ConnectionManager::instance().getById(id);
    if (!conn) return;

    m_nameEdit->setText(conn->name);
    m_typeCombo->setCurrentIndex(m_typeCombo->findData(ConnectionManager::connTypeToString(conn->type)));
    m_dbTypeCombo->setCurrentIndex(m_dbTypeCombo->findData(ConnectionManager::dbTypeToString(conn->dbType)));
    m_hostEdit->setText(conn->host);
    m_portSpin->setValue(conn->port);
    // 数据库连接串：优先显示完整串，否则由 host:port/database 合成
    if (!conn->connectionString.isEmpty()) {
        m_connStrEdit->setText(conn->connectionString);
    } else if (!conn->host.isEmpty()) {
        QString s = conn->host;
        if (conn->port > 0) s += QString(":%1").arg(conn->port);
        if (!conn->database.isEmpty()) s += "/" + conn->database;
        m_connStrEdit->setText(s);
    }
    m_userEdit->setText(conn->username);
    m_passEdit->setText(conn->password);
    m_dbEdit->setText(conn->database);
    m_authTypeCombo->setCurrentIndex(m_authTypeCombo->findData(conn->authType));
    m_keyPathEdit->setText(conn->privateKey);
    m_instantClientEdit->setText(conn->instantClientPath);
    m_descEdit->setPlainText(conn->description);
}

void ConnectionDialog::parseConnectionInput(const QString& input, QString& host, int& port,
                                            QString& database, QString& connStr) const {
    QString s = input.trimmed();
    QString dbType = m_dbTypeCombo->currentData().toString();
    port = ConnectionManager::defaultPort(dbType);

    if (s.contains('=')) {
        // 完整连接串（ODBC 格式），原样保存；尝试提取 Server/Database 用于列表展示
        connStr = s;
        static const QRegularExpression reServer("(?:Server|Data Source|Host)\\s*=\\s*([^;]+)",
                                                 QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression reDb("(?:Database|Dbq|Initial Catalog)\\s*=\\s*([^;]+)",
                                             QRegularExpression::CaseInsensitiveOption);
        auto mSrv = reServer.match(s);
        if (mSrv.hasMatch()) {
            QString srv = mSrv.captured(1).trimmed();
            // 可能是 host,port 或 host:port
            static const QRegularExpression reHostPort("^([^,:]+)[,:](\\d+)$");
            auto mHp = reHostPort.match(srv);
            if (mHp.hasMatch()) {
                host = mHp.captured(1);
                port = mHp.captured(2).toInt();
            } else {
                host = srv;
            }
        }
        auto mDb = reDb.match(s);
        if (mDb.hasMatch()) database = mDb.captured(1).trimmed();
        return;
    }

    // 简式：host[:port][/database]
    static const QRegularExpression reSimple("^([^:/]+)(?::(\\d+))?(?:/(.*))?$");
    auto m = reSimple.match(s);
    if (m.hasMatch()) {
        host = m.captured(1);
        if (!m.captured(2).isEmpty()) port = m.captured(2).toInt();
        database = m.captured(3);
    } else {
        host = s;
    }
}

QVariantMap ConnectionDialog::formData() const {
    QVariantMap data;
    QString type = m_typeCombo->currentData().toString();
    QString dbType = m_dbTypeCombo->currentData().toString();
    data["name"] = m_nameEdit->text();
    data["type"] = type;
    data["dbType"] = dbType;
    data["username"] = m_userEdit->text();
    data["password"] = m_passEdit->text();
    data["authType"] = m_authTypeCombo->currentData().toString();
    data["privateKey"] = m_keyPathEdit->text();
    data["instantClientPath"] = m_instantClientEdit->text();
    data["description"] = m_descEdit->toPlainText();

    if (type == "database" && dbType != "sqlite") {
        // 从连接串解析
        QString host, database, connStr;
        int port = 0;
        parseConnectionInput(m_connStrEdit->text(), host, port, database, connStr);
        data["host"] = host;
        data["port"] = port;
        data["database"] = database;
        data["connectionString"] = connStr;
    } else {
        data["host"] = m_hostEdit->text();
        data["port"] = m_portSpin->value();
        data["database"] = m_dbEdit->text();
        data["connectionString"] = QString();
    }
    return data;
}

void ConnectionDialog::accept() {
    // 校验必填字段
    if (m_nameEdit->text().isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入连接名称");
        m_nameEdit->setFocus();
        return;
    }
    QString type = m_typeCombo->currentData().toString();
    QString dbType = m_dbTypeCombo->currentData().toString();
    if (type == "database" && dbType != "sqlite") {
        if (m_connStrEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "提示", "请输入数据库连接（host:端口/库名 或完整连接串）");
            m_connStrEdit->setFocus();
            return;
        }
    } else if (type != "database" || dbType != "sqlite") {
        if (m_hostEdit->text().isEmpty()) {
            QMessageBox::warning(this, "提示", "请输入主机地址");
            m_hostEdit->setFocus();
            return;
        }
    }
    QDialog::accept();
}

// ── 测试连接 ────────────────────────────────────────────────────────────────

void ConnectionDialog::testConnection() {
    QString type = m_typeCombo->currentData().toString();
    QString dbType = m_dbTypeCombo->currentData().toString();
    QString host = m_hostEdit->text().trimmed();
    int port = m_portSpin->value();
    bool isSqlite = (type == "database" && dbType == "sqlite");
    bool useConnStr = (type == "database" && !isSqlite);

    // 基本校验
    if (isSqlite) {
        if (m_dbEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "测试连接", "请先填写 SQLite 文件路径");
            return;
        }
    } else if (useConnStr) {
        if (m_connStrEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "测试连接", "请先填写数据库连接");
            return;
        }
        QString db, connStr;
        parseConnectionInput(m_connStrEdit->text(), host, port, db, connStr);
    } else if (host.isEmpty()) {
        QMessageBox::warning(this, "测试连接", "请先填写主机地址");
        return;
    }

    // 测试期间禁用按钮并刷新 UI
    m_testBtn->setEnabled(false);
    m_testBtn->setText("测试中...");
    QCoreApplication::processEvents();

    QString detail;
    bool ok = false;

    if (type == "database") {
        ok = testDatabase(&detail);
    } else if (type == "ssh") {
        ok = testSshService(host, port, &detail);
    } else if (type == "redis") {
        ok = testRedisService(host, port, &detail);
    } else if (type == "rdp") {
        ok = testTcpPort(host, port, &detail);
    }

    m_testBtn->setEnabled(true);
    m_testBtn->setText("测试连接");

    if (ok) {
        Logger::instance().success(QString("测试连接成功: %1:%2").arg(host).arg(port), "connections");
        QMessageBox::information(this, "测试连接",
            QString("连接成功！\n\n%1").arg(detail.isEmpty() ? QString("%1:%2 可达").arg(host).arg(port) : detail));
    } else {
        Logger::instance().error(QString("测试连接失败: %1:%2 - %3").arg(host).arg(port).arg(detail), "connections");
        QMessageBox::warning(this, "测试连接", QString("连接失败：\n\n%1").arg(detail));
    }
}

bool ConnectionDialog::testTcpPort(const QString& host, int port, QString* err) {
    QTcpSocket socket;
    socket.connectToHost(host, port);
    if (socket.waitForConnected(5000)) return true;
    *err = QString("无法连接 %1:%2（%3）")
                .arg(host).arg(port).arg(socket.errorString());
    return false;
}

bool ConnectionDialog::testSshService(const QString& host, int port, QString* info) {
    QString err;
    if (!testTcpPort(host, port, &err)) {
        *info = err;
        return false;
    }
    // TCP 已连通，尝试读取 SSH banner 获取服务版本
    QTcpSocket socket;
    socket.connectToHost(host, port);
    socket.waitForConnected(5000);
    if (socket.waitForReadyRead(4000)) {
        QByteArray banner = socket.readAll();
        if (banner.startsWith("SSH-")) {
            *info = QString("%1:%2\n%3").arg(host).arg(port)
                        .arg(QString::fromUtf8(banner.trimmed()));
        } else {
            *info = QString("%1:%2 可达（未收到 SSH 标识，请确认端口）").arg(host).arg(port);
        }
    } else {
        *info = QString("%1:%2 可达（服务未响应标识）").arg(host).arg(port);
    }
    return true;
}

bool ConnectionDialog::testRedisService(const QString& host, int port, QString* err) {
    QString tcpErr;
    if (!testTcpPort(host, port, &tcpErr)) {
        *err = tcpErr;
        return false;
    }
    // 发送 PING 验证 Redis 协议响应
    QTcpSocket socket;
    socket.connectToHost(host, port);
    socket.waitForConnected(5000);
    socket.write("PING\r\n");
    socket.waitForBytesWritten(3000);
    if (!socket.waitForReadyRead(4000)) {
        *err = QString("%1:%2 可达，但无协议响应（可能不是 Redis）").arg(host).arg(port);
        return false;
    }
    QString reply = QString::fromUtf8(socket.readAll().trimmed());
    if (reply.startsWith("+PONG")) {
        *err = QString("%1:%2 响应正常").arg(host).arg(port);
        return true;
    }
    if (reply.startsWith("-NOAUTH")) {
        *err = QString("%1:%2 是 Redis 服务，但需要认证（请检查密码）").arg(host).arg(port);
        return false;
    }
    *err = QString("%1:%2 响应异常: %3").arg(host).arg(port).arg(reply);
    return false;
}

bool ConnectionDialog::testDatabase(QString* err) {
    QString dbType = m_dbTypeCombo->currentData().toString();
    QString user = m_userEdit->text().trimmed();
    QString pass = m_passEdit->text();

    // 数据库（非 SQLite）：从「数据库连接」输入解析
    QString host, database, connStr;
    int port = 0;
    if (dbType != "sqlite") {
        parseConnectionInput(m_connStrEdit->text(), host, port, database, connStr);
    } else {
        database = m_dbEdit->text().trimmed();
    }

    // 驱动映射（与 DatabaseClient 一致）
    QString driver;
    if (dbType == "mysql") {
        QStringList drivers = QSqlDatabase::drivers();
        driver = drivers.contains("QMYSQL") ? "QMYSQL" : "QODBC";
    }
    else if (dbType == "postgres") driver = "QPSQL";
    else if (dbType == "sqlite") driver = "QSQLITE";
    else if (dbType == "mssql") driver = "QODBC";
    else if (dbType == "oracle") {
        QStringList drivers = QSqlDatabase::drivers();
        driver = drivers.contains("QOCI") ? "QOCI" : "QODBC";
    }

    if (driver.isEmpty()) {
        *err = QString("不支持的数据库类型: %1").arg(dbType);
        return false;
    }
    if (!QSqlDatabase::drivers().contains(driver)) {
        // PostgreSQL：插件已编译但缺 libpq 运行库时提供自动下载（安装后需重启生效）
        if (dbType == "postgres") {
            DriverInstaller::ensurePostgresRuntime(this);
        }
        *err = QString("Qt SQL 驱动 %1 不可用。\n已安装的驱动: %2")
                    .arg(driver, QSqlDatabase::drivers().join(", "));
        return false;
    }

    // SQLite：本地文件
    if (dbType == "sqlite") {
        if (!QFile::exists(database)) {
            *err = QString("SQLite 文件不存在: %1").arg(database);
            return false;
        }
    }

    // ODBC 驱动检查：缺失时提供自动下载安装
    if (driver == "QODBC" && (dbType == "oracle" || dbType == "mssql" || dbType == "mysql")) {
        if (!DriverInstaller::ensureOdbcDriver(dbType, this)) {
            *err = "ODBC 驱动不可用（已取消自动安装）";
            return false;
        }
        // 自动安装成功后，把 Instant Client 路径回填到表单，便于查看和保存
        if (dbType == "oracle" && m_instantClientEdit->text().trimmed().isEmpty()
            && DriverInstaller::isLocalInstantClientReady()) {
            m_instantClientEdit->setText(DriverInstaller::instantClientDir());
        }
    }

    // ODBC 连接串（与 DatabaseClient 一致）
    QString odbcConnStr;
    if (driver == "QODBC") {
        if (!connStr.isEmpty()) {
            // 用户提供了完整连接串，补全凭据
            odbcConnStr = connStr;
            if (!odbcConnStr.contains("Uid=", Qt::CaseInsensitive) && !user.isEmpty()) {
                if (!odbcConnStr.endsWith(';')) odbcConnStr += ';';
                odbcConnStr += "Uid=" + user + ";";
            }
            if (!odbcConnStr.contains("Pwd=", Qt::CaseInsensitive)
                && !odbcConnStr.contains("Password=", Qt::CaseInsensitive) && !pass.isEmpty()) {
                if (!odbcConnStr.endsWith(';')) odbcConnStr += ';';
                odbcConnStr += "Pwd=" + pass + ";";
            }
        } else if (dbType == "mssql") {
            QString drv = DriverInstaller::findOdbcDriver("sql server", "odbc driver");
            odbcConnStr = QString("Driver={%1};Server=%2,%3;Database=%4;Uid=%5;Pwd=%6;")
                               .arg(drv).arg(host).arg(port).arg(database, user, pass);
        } else if (dbType == "mysql") {
            QString drv = DriverInstaller::findOdbcDriver("mysql odbc", "unicode");
            odbcConnStr = QString("Driver={%1};Server=%2;Port=%3;Database=%4;UID=%5;PWD=%6;")
                               .arg(drv).arg(host).arg(port).arg(database, user, pass);
        } else {
            QString drv = DriverInstaller::findOdbcDriver("oracle", "instantclient");
            odbcConnStr = QString("Driver={%1};Dbq=%2:%3/%4;Uid=%5;Pwd=%6;")
                               .arg(drv).arg(host).arg(port).arg(database, user, pass);
        }
    }

    QString connName = "desktoptest_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(driver, connName);
        // drivers() 只读插件元数据，addDatabase 才真正加载插件 DLL；
        // 原生驱动加载失败（如 QOCI 缺 oci.dll）时回退 ODBC 再试
        if (!db.isValid() && (dbType == "oracle" || dbType == "mysql") && driver != "QODBC") {
            QSqlDatabase::removeDatabase(connName);
            driver = "QODBC";
            if (odbcConnStr.isEmpty()) {
                QString drv = DriverInstaller::findOdbcDriver(
                    dbType == "mysql" ? "mysql odbc" : "oracle",
                    dbType == "mysql" ? "unicode" : "instantclient");
                if (!drv.isEmpty()) {
                    odbcConnStr = (dbType == "mysql")
                        ? QString("Driver={%1};Server=%2;Port=%3;Database=%4;UID=%5;PWD=%6;")
                              .arg(drv).arg(host).arg(port).arg(database, user, pass)
                        : QString("Driver={%1};Dbq=%2:%3/%4;Uid=%5;Pwd=%6;")
                              .arg(drv).arg(host).arg(port).arg(database, user, pass);
                }
            }
            db = QSqlDatabase::addDatabase(driver, connName);
        }
        if (!db.isValid()) {
            *err = QString("Qt SQL 驱动 %1 加载失败：插件依赖的运行库缺失\n"
                           "（Oracle 需 Instant Client，PostgreSQL 需 libpq，可在连接时按提示自动安装）")
                       .arg(driver);
            QSqlDatabase::removeDatabase(connName);
            return false;
        }
        if (driver == "QODBC") {
            db.setDatabaseName(odbcConnStr);
        } else {
            db.setHostName(host);
            db.setPort(port);
            db.setUserName(user);
            db.setPassword(pass);
            db.setDatabaseName(database);
            if (dbType == "sqlite") {
                // 只读打开，避免测试时意外创建/修改文件
                db.setConnectOptions("QSQLITE_OPEN_READONLY=1");
            }
        }
        ok = db.open();
        if (!ok) {
            *err = QString("%1 连接失败:\n%2").arg(dbType.toUpper()).arg(db.lastError().text());
        } else {
            *err = QString("%1 连接成功（%2）")
                        .arg(dbType.toUpper())
                        .arg(dbType == "sqlite" ? database : QString("%1:%2/%3").arg(host).arg(port).arg(database));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
    return ok;
}

// ── AppTypeSelectorDialog: 选择应用类型页面 ──────────────────────────────────

// 卡片式按钮（图标 + 标题 + 副标题）
static QPushButton* makeAppCard(const QString& icon, const QString& title,
                                const QString& subtitle) {
    auto* btn = new QPushButton;
    btn->setFixedSize(200, 90);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(
        "QPushButton { background: #252830; border: 1px solid #3a3d46; border-radius: 10px; "
        "text-align: left; padding: 10px 16px; color: #c8c8c8; font-size: 14px; }"
        "QPushButton:hover { background: #2a2d3a; border: 1px solid #4fc3f7; }"
        "QPushButton:pressed { background: #1e2128; }");
    btn->setText(QString("%1  %2\n%3").arg(icon, title, subtitle.isEmpty() ? QString() : "    " + subtitle));
    // 使用富文本式换行需要 label，这里用两行布局更清晰
    auto* iconLbl = new QLabel(icon);
    iconLbl->setStyleSheet("font-size: 28px; background: transparent; border: none;");
    iconLbl->setAlignment(Qt::AlignCenter);
    auto* titleLbl = new QLabel(title);
    titleLbl->setStyleSheet("font-size: 14px; font-weight: bold; color: #c8c8c8; background: transparent; border: none;");
    auto* subLbl = new QLabel(subtitle);
    subLbl->setStyleSheet("font-size: 11px; color: #8a8a8a; background: transparent; border: none;");
    auto* textCol = new QVBoxLayout;
    textCol->setContentsMargins(0, 0, 0, 0);
    textCol->setSpacing(2);
    textCol->addWidget(titleLbl);
    if (!subtitle.isEmpty()) textCol->addWidget(subLbl);
    textCol->addStretch();
    auto* row = new QHBoxLayout(btn);
    row->setContentsMargins(8, 8, 8, 8);
    row->setSpacing(10);
    row->addWidget(iconLbl);
    row->addLayout(textCol, 1);
    return btn;
}

AppTypeSelectorDialog::AppTypeSelectorDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("选择应用类型");
    setMinimumWidth(520);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(16);

    m_titleLbl = new QLabel("请选择要添加的应用类型");
    m_titleLbl->setStyleSheet("font-size: 16px; color: #c8c8c8; font-weight: bold;");
    layout->addWidget(m_titleLbl);

    m_stack = new QStackedWidget;
    layout->addWidget(m_stack, 1);

    // ── 第 1 页：主应用类型卡片 ──
    auto* mainPage = new QWidget;
    auto* mainLayout = new QVBoxLayout(mainPage);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    struct AppItem { QString type; QString icon; QString title; QString subtitle; };
    QList<AppItem> apps = {
        {"ssh",       "🖥", "SSH / SFTP",  "远程终端与文件传输"},
        {"database",  "💾", "数据库",      "MySQL / Oracle / PostgreSQL 等"},
        {"redis",     "🔴", "Redis",       "键值数据库"},
        {"rdp",       "💻", "远程桌面",    "Windows 远程连接"},
        {"mail",      "📧", "邮箱",        "SMTP 邮件服务"},
        {"wechat",    "💬", "微信",        "本机聊天记录 / 通讯录"},
    };

    // 卡片网格（每行 2 个）
    const int cols = 2;
    QGridLayout* grid = new QGridLayout;
    grid->setSpacing(12);
    grid->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < apps.size(); ++i) {
        const auto& app = apps[i];
        auto* card = makeAppCard(app.icon, app.title, app.subtitle);
        grid->addWidget(card, i / cols, i % cols);
        connect(card, &QPushButton::clicked, this, [this, type = app.type]() {
            if (type == "database") {
                // 数据库：进入子页面选择具体数据库类型
                m_stack->setCurrentIndex(1);
                m_titleLbl->setText("选择数据库类型");
                m_backBtn->setVisible(true);
            } else {
                selectApp(type);
            }
        });
    }
    mainLayout->addLayout(grid);
    mainLayout->addStretch();
    m_stack->addWidget(mainPage);

    // ── 第 2 页：数据库子类型 ──
    buildDatabaseSubPage();

    // 底部按钮：返回（仅数据库子页显示）+ 取消
    auto* btnRow = new QHBoxLayout;
    m_backBtn = new QPushButton("← 返回");
    m_backBtn->setCursor(Qt::PointingHandCursor);
    m_backBtn->setVisible(false);
    connect(m_backBtn, &QPushButton::clicked, this, [this]() {
        m_stack->setCurrentIndex(0);
        m_titleLbl->setText("请选择要添加的应用类型");
        m_backBtn->setVisible(false);
    });
    btnRow->addWidget(m_backBtn);
    btnRow->addStretch();
    auto* cancelBtn = new QPushButton("取消");
    cancelBtn->setCursor(Qt::PointingHandCursor);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);
    layout->addLayout(btnRow);
}

void AppTypeSelectorDialog::buildDatabaseSubPage() {
    auto* page = new QWidget;
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    struct DbItem { QString dbType; QString icon; QString title; QString subtitle; };
    QList<DbItem> dbs = {
        {"mysql",    "🐬", "MySQL",       "默认端口 3306"},
        {"oracle",   "🔶", "Oracle",      "默认端口 1521"},
        {"postgres", "🐘", "PostgreSQL",  "默认端口 5432"},
        {"mssql",    "🟦", "SQL Server",  "默认端口 1433"},
        {"sqlite",   "📄", "SQLite",      "本地数据库文件"},
    };

    const int cols = 2;
    QGridLayout* grid = new QGridLayout;
    grid->setSpacing(12);
    grid->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < dbs.size(); ++i) {
        const auto& db = dbs[i];
        auto* card = makeAppCard(db.icon, db.title, db.subtitle);
        grid->addWidget(card, i / cols, i % cols);
        connect(card, &QPushButton::clicked, this, [this, dbType = db.dbType]() {
            selectApp("database", dbType);
        });
    }
    pageLayout->addLayout(grid);
    pageLayout->addStretch();
    m_stack->addWidget(page);
}

void AppTypeSelectorDialog::selectApp(const QString& type, const QString& dbType) {
    m_type = type;
    m_dbType = dbType;
    accept();
}
