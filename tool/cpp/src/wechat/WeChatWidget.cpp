#include "WeChatWidget.h"
#include "WeChatAccountManager.h"
#include "WeChatConfigDialog.h"
#include "WeChatDb.h"
#include "wechat/ui/WeChatSidebar.h"
#include "wechat/ui/WeChatDetailPanel.h"
#include "app/Theme.h"
#include "core/Logger.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>

WeChatWidget::WeChatWidget(QWidget* parent) : QWidget(parent) {
    buildUi();
    connect(&WeChatAccountManager::instance(), &WeChatAccountManager::changed,
            this, &WeChatWidget::onAccountsChanged);
    onAccountsChanged();
}

void WeChatWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 顶部工具栏 ──
    auto* top = new QWidget;
    top->setFixedHeight(40);
    top->setStyleSheet(QString("background:%1; border-bottom:1px solid %2;")
                           .arg(Theme::kBg, Theme::kBorder));
    auto* topLay = new QHBoxLayout(top);
    topLay->setContentsMargins(16, 0, 16, 0);
    topLay->setSpacing(8);
    auto* title = new QLabel("微信");
    title->setStyleSheet(QString("font-size:15px; font-weight:600; color:%1;")
                             .arg(Theme::kTextBright));
    topLay->addWidget(title);
    topLay->addStretch(1);
    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet(Theme::mutedText());
    topLay->addWidget(m_statusLabel);
    root->addWidget(top);

    // ── 主堆叠：page 0 空账号引导；page 1 工作 ──
    m_mainStack = new QStackedWidget;
    root->addWidget(m_mainStack, 1);

    // ── 空账号引导页 ──
    m_emptyPage = new QWidget;
    m_emptyPage->setStyleSheet(QString("background:%1;").arg(Theme::kBg));
    auto* emptyL = new QVBoxLayout(m_emptyPage);
    emptyL->setContentsMargins(0, 0, 0, 0);
    emptyL->setAlignment(Qt::AlignCenter);
    auto* emptyCard = new QFrame;
    emptyCard->setObjectName("wechatEmptyCard");
    emptyCard->setFrameShape(QFrame::StyledPanel);
    emptyCard->setMinimumWidth(420);
    emptyCard->setMaximumWidth(560);
    emptyCard->setStyleSheet(QString(
        "#wechatEmptyCard { background: %1; border: 1px solid %2; border-radius: 12px; }")
        .arg(Theme::kSidebar, Theme::kBorder));
    auto* cardL = new QVBoxLayout(emptyCard);
    cardL->setContentsMargins(40, 36, 40, 36);
    cardL->setSpacing(14);
    cardL->setAlignment(Qt::AlignHCenter);
    auto* bigIcon = new QLabel("\xF0\x9F\x93\xB1");   // 📱
    bigIcon->setAlignment(Qt::AlignCenter);
    bigIcon->setStyleSheet(QString("font-size: 64px; color: %1;").arg(Theme::kCatWeChat));
    cardL->addWidget(bigIcon);
    auto* titleLbl = new QLabel("尚未添加微信账号");
    titleLbl->setAlignment(Qt::AlignCenter);
    titleLbl->setStyleSheet(QString("font-size: 20px; font-weight: bold; color: %1;")
                                .arg(Theme::kTextBright));
    cardL->addWidget(titleLbl);
    auto* subLbl = new QLabel(
        "添加本机微信账号后可在此查看会话、联系人与聊天记录。\n"
        "支持多账号管理，每账号独立密钥，使用 Windows DPAPI 加密保存。\n"
        "微信登录状态下密钥可自动从进程内存提取。");
    subLbl->setAlignment(Qt::AlignCenter);
    subLbl->setWordWrap(true);
    subLbl->setStyleSheet(QString("color:%1; font-size: 13px; line-height: 1.6;")
                              .arg(Theme::kMuted));
    cardL->addWidget(subLbl);
    auto* addBtn = new QPushButton("+  添加微信账号");
    addBtn->setCursor(Qt::PointingHandCursor);
    addBtn->setMinimumHeight(40);
    addBtn->setMinimumWidth(180);
    addBtn->setStyleSheet(Theme::flatBtnPrimary() +
        " QPushButton { padding: 8px 20px; font-size: 14px; }");
    connect(addBtn, &QPushButton::clicked, this, &WeChatWidget::onAddAccount);
    cardL->addWidget(addBtn, 0, Qt::AlignHCenter);
    emptyL->addWidget(emptyCard, 0, Qt::AlignCenter);
    m_mainStack->addWidget(m_emptyPage);

    // ── 工作页 ──
    m_workPage = new QWidget;
    auto* workRoot = new QVBoxLayout(m_workPage);
    workRoot->setContentsMargins(0, 0, 0, 0);
    workRoot->setSpacing(0);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    m_sidebar = new WeChatSidebar;
    m_sidebar->setMinimumWidth(220);
    m_detailPanel = new WeChatDetailPanel;
    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(m_detailPanel);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({280, 800});
    workRoot->addWidget(m_splitter, 1);

    // ── 底部状态栏 ──
    auto* statusBar = new QWidget;
    statusBar->setFixedHeight(24);
    statusBar->setStyleSheet(QString("background:%1; border-top:1px solid %2;")
                                 .arg(Theme::kBg, Theme::kBorder));
    auto* statusLay = new QHBoxLayout(statusBar);
    statusLay->setContentsMargins(12, 0, 12, 0);
    auto* statusHint = new QLabel("右键账号或侧边栏空白处可添加 / 编辑微信账号");
    statusHint->setStyleSheet(QString("color:%1; font-size:11px;").arg(Theme::kFaint));
    statusLay->addWidget(statusHint);
    statusLay->addStretch(1);
    workRoot->addWidget(statusBar);

    m_mainStack->addWidget(m_workPage);

    // ── 侧边栏信号 ──
    connect(m_sidebar, &WeChatSidebar::addAccountRequested,
            this, &WeChatWidget::onAddAccount);
    connect(m_sidebar, &WeChatSidebar::editAccountRequested,
            this, &WeChatWidget::onEditAccount);
    connect(m_sidebar, &WeChatSidebar::deleteAccountRequested,
            this, &WeChatWidget::onDeleteAccount);
    connect(m_sidebar, &WeChatSidebar::refreshRequested,
            this, &WeChatWidget::onRefreshCurrent);
    connect(m_sidebar, &WeChatSidebar::loadSessionsRequested,
            this, &WeChatWidget::onSidebarLoadSessions);
    connect(m_sidebar, &WeChatSidebar::loadContactsRequested,
            this, &WeChatWidget::onSidebarLoadContacts);
    connect(m_sidebar, &WeChatSidebar::openChatRequested,
            this, &WeChatWidget::onSidebarOpenChat);
    connect(m_sidebar, &WeChatSidebar::showContactRequested,
            this, &WeChatWidget::onSidebarShowContact);
}

void WeChatWidget::setStatus(const QString& text) {
    if (m_statusLabel) m_statusLabel->setText(text);
}

void WeChatWidget::updateEmptyState() {
    const bool empty = WeChatAccountManager::instance().accounts().isEmpty();
    m_mainStack->setCurrentIndex(empty ? 0 : 1);
}

// ── 账号变化 / 配置入口 ────────────────────────────────────────────────────

void WeChatWidget::onAccountsChanged() {
    updateEmptyState();
    if (m_sidebar) m_sidebar->rebuildTree(m_currentAccountId);
    setStatus(QString("账号数：%1").arg(WeChatAccountManager::instance().accounts().size()));
}

void WeChatWidget::openConfig(const QString& editId) {
    WeChatConfigDialog dlg(this, editId);
    if (dlg.exec() == QDialog::Accepted) {
        // changed() 信号会触发 onAccountsChanged → 自动刷新
    }
}

void WeChatWidget::selectAccount(const QString& accountId) {
    if (accountId.isEmpty()) return;
    if (!WeChatAccountManager::instance().getById(accountId)) return;
    m_currentAccountId = accountId;
    updateEmptyState();
    if (m_sidebar) m_sidebar->selectAccount(accountId);
}

// ── 侧边栏操作 ─────────────────────────────────────────────────────────────

void WeChatWidget::onAddAccount() {
    openConfig();
}

void WeChatWidget::onEditAccount(const QString& accId) {
    if (accId.isEmpty()) return;
    if (!WeChatAccountManager::instance().getById(accId)) return;
    openConfig(accId);
}

void WeChatWidget::onDeleteAccount(const QString& accId) {
    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) return;
    const auto ret = QMessageBox::question(
        this, "删除微信账号",
        QString("确定删除账号「%1」（%2）？\n该操作不会删除本机微信数据文件。")
            .arg(acc->name, acc->wxid),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;
    WeChatAccountManager::instance().remove(accId);
    // changed() 信号会触发 onAccountsChanged 自动重建
}

void WeChatWidget::onRefreshCurrent() {
    if (m_currentAccountId.isEmpty()) return;
    if (m_sidebar) m_sidebar->clearData(m_currentAccountId);
    m_sessionsCache.remove(m_currentAccountId);
    m_contactsCache.remove(m_currentAccountId);
    onSidebarLoadContacts(m_currentAccountId);
    onSidebarLoadSessions(m_currentAccountId);
    setStatus("已刷新当前账号");
}

// ── 数据加载（按账号缓存） ─────────────────────────────────────────────────

bool WeChatWidget::loadAccountData(const QString& accId) {
    if (accId.isEmpty()) return false;
    if (m_sessionsCache.contains(accId) && m_contactsCache.contains(accId)) return true;

    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) return false;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    setStatus(QString("正在解密 %1 数据库…").arg(acc->name));

    WeChatDb db(acc->id, acc->dataDir, WeChatAccountManager::instance().keyForAccount(*acc));
    bool ok = db.ensureDecrypted();
    if (ok) {
        QVariantList sl;
        for (const auto& s : db.loadSessions()) {
            QVariantMap vm;
            vm["talker"]  = s.talker;
            vm["title"]   = s.title;
            vm["lastMsg"] = s.lastMsg;
            vm["time"]    = s.lastTime;
            vm["unread"]  = s.unread;
            vm["isRoom"]  = s.isChatRoom;
            sl.append(vm);
        }
        m_sessionsCache.insert(accId, sl);
        if (m_sidebar) m_sidebar->setSessions(accId, sl);

        QVariantList cl;
        for (const auto& c : db.loadContacts()) {
            QVariantMap vm;
            vm["userName"] = c.userName;
            vm["display"]  = c.display;
            vm["remark"]   = c.remark;
            vm["nickname"] = c.nickname;
            vm["alias"]    = c.alias;
            vm["isRoom"]   = c.isChatRoom;
            cl.append(vm);
        }
        m_contactsCache.insert(accId, cl);
        if (m_sidebar) m_sidebar->setContacts(accId, cl);

        setStatus(QString("%1 · %2 个会话 · %3 个联系人")
                      .arg(acc->name).arg(sl.size()).arg(cl.size()));
    } else {
        setStatus("加载失败（请检查密钥或数据目录）");
    }
    QApplication::restoreOverrideCursor();
    return ok;
}

void WeChatWidget::onSidebarLoadSessions(const QString& accId) {
    if (accId.isEmpty()) return;
    loadAccountData(accId);
}

void WeChatWidget::onSidebarLoadContacts(const QString& accId) {
    if (accId.isEmpty()) return;
    loadAccountData(accId);
}

void WeChatWidget::onSidebarOpenChat(const QString& accId, const QString& talker) {
    if (talker.isEmpty() || accId.isEmpty()) return;
    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) return;

    m_currentAccountId = accId;
    m_currentTalker = talker;
    if (!loadAccountData(accId)) {
        m_detailPanel->showEmpty("数据库解密失败");
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    WeChatDb db(acc->id, acc->dataDir, WeChatAccountManager::instance().keyForAccount(*acc));
    const auto msgs = db.loadMessages(talker, 500);

    QString title;
    for (const auto& v : m_sessionsCache.value(accId)) {
        const auto s = v.toMap();
        if (s["talker"].toString() == talker) {
            title = s["title"].toString();
            break;
        }
    }
    if (title.isEmpty()) title = db.displayName(talker);
    if (talker.endsWith("@chatroom")) {
        const QStringList members = db.chatRoomMembers(talker);
        title += QString("（%1）").arg(members.size());
    }
    m_detailPanel->showChatHeader(title);

    QList<QVariantMap> vl;
    for (const auto& m : msgs) {
        QVariantMap vm;
        vm["senderName"] = m.senderName;
        vm["senderId"]   = m.senderId;
        vm["isSender"]   = m.isSender;
        vm["type"]       = m.type;
        vm["subType"]    = m.subType;
        vm["content"]    = m.content;
        vm["display"]    = m.display;
        vm["time"]       = m.time;
        vl.append(vm);
    }
    m_detailPanel->renderMessages(vl, talker);
    QApplication::restoreOverrideCursor();
}

void WeChatWidget::onSidebarShowContact(const QString& accId, const QString& wxid) {
    if (accId.isEmpty() || wxid.isEmpty()) return;
    m_currentAccountId = accId;
    if (!loadAccountData(accId)) {
        m_detailPanel->showEmpty("数据库解密失败");
        return;
    }
    const auto& list = m_contactsCache.value(accId);
    for (const auto& v : list) {
        const auto c = v.toMap();
        if (c["userName"].toString() == wxid) {
            m_detailPanel->showContact(c);
            return;
        }
    }
}