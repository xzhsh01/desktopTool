#include "WeChatWidget.h"
#include "WeChatAccountManager.h"
#include "WeChatConfigDialog.h"
#include "WeChatDb.h"
#include "WeChatWorker.h"
#include "wechat/ui/WeChatSidebar.h"
#include "wechat/ui/WeChatDetailPanel.h"
#include "wechat/ui/WeChatListPanel.h"
#include "app/Theme.h"
#include "core/Logger.h"

#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

WeChatWidget::WeChatWidget(QWidget* parent) : QWidget(parent) {
    buildUi();
    startWorker();
    connect(&WeChatAccountManager::instance(), &WeChatAccountManager::changed,
            this, &WeChatWidget::onAccountsChanged);
    onAccountsChanged();
}

WeChatWidget::~WeChatWidget() {
    stopWorker();
}

void WeChatWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 顶部已去除（按需取消整行工具栏） ──

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
    m_sidebar->setMinimumWidth(200);
    m_listPanel = new WeChatListPanel;
    m_listPanel->setMinimumWidth(240);
    m_detailPanel = new WeChatDetailPanel;
    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(m_listPanel);
    m_splitter->addWidget(m_detailPanel);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 1);
    m_splitter->setSizes({240, 320, 700});
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

    // 文件夹点击 → 中栏切页 + 触发数据加载
    connect(m_sidebar, &WeChatSidebar::chatFolderClicked,
            this, &WeChatWidget::onSidebarChatFolderClicked);
    connect(m_sidebar, &WeChatSidebar::contactFolderClicked,
            this, &WeChatWidget::onSidebarContactFolderClicked);

    // 中栏项点击 → 打开聊天 / 联系人详情
    connect(m_listPanel, &WeChatListPanel::chatItemClicked,
            this, &WeChatWidget::onListOpenChat);
    connect(m_listPanel, &WeChatListPanel::contactItemClicked,
            this, &WeChatWidget::onListShowContact);
}

void WeChatWidget::setStatus(const QString& text) {
    Q_UNUSED(text);
    // 顶部状态标签已移除（按需取消顶部工具栏）
}

// ── 后台线程管理 ─────────────────────────────────────────────────────────────

void WeChatWidget::startWorker() {
    m_loadThread = new QThread(this);
    m_loadWorker = new WeChatWorker;          // 无父对象，由 thread 负责 delete
    m_loadWorker->moveToThread(m_loadThread);
    connect(m_loadThread, &QThread::finished,
            m_loadWorker, &QObject::deleteLater);
    connect(m_loadWorker, &WeChatWorker::accountLoaded,
            this, &WeChatWidget::onAccountLoaded);
    connect(m_loadWorker, &WeChatWorker::accountFailed,
            this, &WeChatWidget::onAccountFailed);
    connect(m_loadWorker, &WeChatWorker::messagesLoaded,
            this, &WeChatWidget::onMessagesLoaded);
    connect(m_loadWorker, &WeChatWorker::messagesFailed,
            this, &WeChatWidget::onMessagesFailed);
    m_loadThread->start();
    Logger::instance().info("WeChatWidget worker thread started", "wechat");
}

void WeChatWidget::stopWorker() {
    if (!m_loadThread) return;
    m_loadThread->quit();
    if (!m_loadThread->wait(3000)) {
        Logger::instance().warn("WeChatWidget worker thread didn't exit in 3s, terminating",
                                "wechat");
        m_loadThread->terminate();
        m_loadThread->wait();
    }
    m_loadThread = nullptr;     // 由 this 父对象析构负责 delete
    m_loadWorker = nullptr;
}

void WeChatWidget::updateEmptyState() {
    const bool empty = WeChatAccountManager::instance().accounts().isEmpty();
    m_mainStack->setCurrentIndex(empty ? 0 : 1);
}

// ── 账号变化 / 配置入口 ────────────────────────────────────────────────────

void WeChatWidget::onAccountsChanged() {
    updateEmptyState();
    // 账号列表变化后，若当前选中账号已失效，重置为第一个账号
    const auto& accs = WeChatAccountManager::instance().accounts();
    if (m_currentAccountId.isEmpty() ||
        !WeChatAccountManager::instance().getById(m_currentAccountId)) {
        if (!accs.isEmpty()) {
            m_currentAccountId = accs.first().id;
            if (m_listPanel) m_listPanel->setCurrentAccId(m_currentAccountId);
        }
    }
    if (m_sidebar) m_sidebar->rebuildTree(m_currentAccountId);
    setStatus(QString("账号数：%1").arg(accs.size()));
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
    if (m_listPanel) m_listPanel->setCurrentAccId(accountId);
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
    m_sessionsCache.remove(m_currentAccountId);
    m_contactsCache.remove(m_currentAccountId);
    if (m_listPanel) m_listPanel->clearData(m_currentAccountId);
    loadAccountData(m_currentAccountId);
    setStatus("已刷新当前账号");
}

// ── 数据加载（按账号缓存） ─────────────────────────────────────────────────

bool WeChatWidget::loadAccountData(const QString& accId) {
    if (accId.isEmpty()) return false;
    // 缓存命中：直接返回成功（不再触发后台 worker）
    if (m_sessionsCache.contains(accId) && m_contactsCache.contains(accId)) return true;

    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) return false;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    setStatus(QString("正在解密 %1 数据库…").arg(acc->name));

    // 提交后台线程执行（不阻塞 UI）
    QMetaObject::invokeMethod(m_loadWorker, "loadAccountData",
                              Qt::QueuedConnection,
                              Q_ARG(QString, accId));
    return true;        // 表示"任务已提交"，由回调 onAccountLoaded/onAccountFailed 完成状态更新
}

// ── sidebar 文件夹点击 → 中栏切页 + 触发加载 ──

void WeChatWidget::onSidebarChatFolderClicked(const QString& accId) {
    if (accId.isEmpty()) return;
    m_currentAccountId = accId;
    if (m_listPanel) {
        m_listPanel->setCurrentAccId(accId);
        m_listPanel->showChatList();
    }
    // 触发数据加载（命中缓存直接返回；未命中交给 worker）
    loadAccountData(accId);
}

void WeChatWidget::onSidebarContactFolderClicked(const QString& accId) {
    if (accId.isEmpty()) return;
    m_currentAccountId = accId;
    if (m_listPanel) {
        m_listPanel->setCurrentAccId(accId);
        m_listPanel->showContactList();
    }
    loadAccountData(accId);
}

// ── 中栏列表项点击 → 打开聊天 / 联系人详情 ──

void WeChatWidget::onListOpenChat(const QString& accId, const QString& talker) {
    if (talker.isEmpty() || accId.isEmpty()) return;
    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) return;

    m_currentAccountId = accId;
    m_currentTalker = talker;

    // 先确保账号数据已加载（同步等待缓存，或交给 worker 异步加载）
    if (!m_sessionsCache.contains(accId) || !m_contactsCache.contains(accId)) {
        // 没缓存 → 走 worker 异步加载账号数据；onAccountLoaded 里再触发 loadMessages
        QApplication::setOverrideCursor(Qt::WaitCursor);
        setStatus(QString("正在解密 %1 数据库…").arg(acc->name));
        QMetaObject::invokeMethod(m_loadWorker, "loadAccountData",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, accId));
        return;
    }

    // 有缓存 → 直接后台线程加载消息
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QMetaObject::invokeMethod(m_loadWorker, "loadMessages",
                              Qt::QueuedConnection,
                              Q_ARG(QString, accId),
                              Q_ARG(QString, talker),
                              Q_ARG(int, 500));
}

void WeChatWidget::onListShowContact(const QString& accId, const QString& wxid) {
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

// ── worker 完成回调（主线程） ───────────────────────────────────────────────

void WeChatWidget::onAccountLoaded(const QString& accId,
                                    const QVariantList& sessions,
                                    const QVariantList& contacts) {
    QApplication::restoreOverrideCursor();
    m_sessionsCache.insert(accId, sessions);
    m_contactsCache.insert(accId, contacts);
    if (m_listPanel) {
        m_listPanel->setSessions(accId, sessions);
        m_listPanel->setContacts(accId, contacts);
    }

    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (acc) {
        setStatus(QString("%1 · %2 个会话 · %3 个联系人")
                      .arg(acc->name).arg(sessions.size()).arg(contacts.size()));
    }

    // 如果用户已点了某个 talker 但还没触发消息加载 → 现在补上
    if (m_currentAccountId == accId && !m_currentTalker.isEmpty()) {
        QMetaObject::invokeMethod(m_loadWorker, "loadMessages",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, accId),
                                  Q_ARG(QString, m_currentTalker),
                                  Q_ARG(int, 500));
    }
}

void WeChatWidget::onAccountFailed(const QString& accId, const QString& reason) {
    QApplication::restoreOverrideCursor();
    setStatus(QString("加载失败：%1").arg(reason));
    if (m_currentAccountId == accId && !m_currentTalker.isEmpty()) {
        m_detailPanel->showEmpty("数据库解密失败");
    }
}

void WeChatWidget::onMessagesLoaded(const QString& accId,
                                     const QString& talker,
                                     const QString& title,
                                     const QList<QVariantMap>& messages) {
    QApplication::restoreOverrideCursor();
    if (m_currentAccountId != accId || m_currentTalker != talker) return;
    m_detailPanel->showChatHeader(title);
    m_detailPanel->renderMessages(messages, talker);
}

void WeChatWidget::onMessagesFailed(const QString& accId,
                                     const QString& talker,
                                     const QString& reason) {
    QApplication::restoreOverrideCursor();
    if (m_currentAccountId != accId || m_currentTalker != talker) return;
    m_detailPanel->showEmpty(reason);
}