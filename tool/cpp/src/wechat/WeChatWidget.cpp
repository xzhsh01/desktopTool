#include "WeChatWidget.h"
#include "CacheDb.h"
#include "WeChatAccountManager.h"
#include "WeChatConfigDialog.h"
#include "WeChatDb.h"
#include "WeChatSyncWorker.h"
#include "wechat/ui/WeChatSidebar.h"
#include "wechat/ui/WeChatDetailPanel.h"
#include "wechat/ui/WeChatListPanel.h"
#include "app/Theme.h"
#include "core/Logger.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
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
    // 1) 初始化本地缓存库
    QString err;
    if (!CacheDb::initialize(&err)) {
        Logger::instance().error(
            QString("CacheDb 初始化失败: %1").arg(err), "wechat");
    }

    buildUi();
    startSyncWorker();
    connect(&WeChatAccountManager::instance(), &WeChatAccountManager::changed,
            this, &WeChatWidget::onAccountsChanged);
    onAccountsChanged();
}

WeChatWidget::~WeChatWidget() {
    stopSyncWorker();
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

    // ── 底部状态栏（流式显示同步进度） ──
    auto* statusBar = new QWidget;
    statusBar->setFixedHeight(26);
    statusBar->setStyleSheet(QString("background:%1; border-top:1px solid %2;")
                                 .arg(Theme::kBg, Theme::kBorder));
    auto* statusLay = new QHBoxLayout(statusBar);
    statusLay->setContentsMargins(12, 0, 12, 0);
    m_statusLabel = new QLabel("就绪");
    m_statusLabel->setStyleSheet(QString("color:%1; font-size:11px;").arg(Theme::kFaint));
    statusLay->addWidget(m_statusLabel);
    statusLay->addStretch(1);
    workRoot->addWidget(statusBar);

    // 状态栏节流：syncProgress 频率很高，16ms 节流到一次刷新
    m_statusTickTimer = new QTimer(this);
    m_statusTickTimer->setSingleShot(true);
    m_statusTickTimer->setInterval(120);
    connect(m_statusTickTimer, &QTimer::timeout, this, &WeChatWidget::onStatusTick);

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

void WeChatWidget::setStatusText(const QString& text) {
    m_pendingStatusText = text;
    if (m_statusTickTimer && !m_statusTickTimer->isActive()) {
        m_statusTickTimer->start();
    }
}

void WeChatWidget::onStatusTick() {
    if (m_statusLabel) m_statusLabel->setText(m_pendingStatusText);
}

void WeChatWidget::updateStatusBar() {
    // 留作扩展：可在账号切换时刷新"X 个账号 · Y 个会话"等综合信息
}

// ── 后台同步线程 ─────────────────────────────────────────────────────────────

void WeChatWidget::startSyncWorker() {
    m_syncThread = new QThread(this);
    m_syncWorker = new WeChatSyncWorker;        // 无父对象，由 thread delete
    m_syncWorker->moveToThread(m_syncThread);

    // worker → UI：Qt 自动 QueuedConnection（跨线程）
    connect(m_syncWorker, &WeChatSyncWorker::syncStarted,
            this, &WeChatWidget::onSyncStarted);
    connect(m_syncWorker, &WeChatSyncWorker::syncProgress,
            this, &WeChatWidget::onSyncProgress);
    connect(m_syncWorker, &WeChatSyncWorker::syncAccountDataReady,
            this, &WeChatWidget::onSyncAccountDataReady);
    connect(m_syncWorker, &WeChatSyncWorker::syncMessagesReady,
            this, &WeChatWidget::onSyncMessagesReady);
    connect(m_syncWorker, &WeChatSyncWorker::syncFinished,
            this, &WeChatWidget::onSyncFinished);
    connect(m_syncWorker, &WeChatSyncWorker::syncFailed,
            this, &WeChatWidget::onSyncFailed);
    // 联系人详细信息异步返回
    connect(m_syncWorker, &WeChatSyncWorker::contactDetailReady,
            this, &WeChatWidget::onContactDetailReady);

    connect(m_syncThread, &QThread::finished,
            m_syncWorker, &QObject::deleteLater);
    m_syncThread->start();
    Logger::instance().info("WeChatSyncWorker thread started", "wechat");

    // 启动后立即异步同步所有账号（不阻塞 UI）
    QMetaObject::invokeMethod(m_syncWorker, "syncAll",
                              Qt::QueuedConnection);
}

void WeChatWidget::stopSyncWorker() {
    if (!m_syncThread) return;
    m_syncThread->quit();
    if (!m_syncThread->wait(3000)) {
        Logger::instance().warn("SyncWorker thread didn't exit in 3s, terminating",
                                "wechat");
        m_syncThread->terminate();
        m_syncThread->wait();
    }
    m_syncThread = nullptr;
    m_syncWorker = nullptr;
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
            // 立刻从缓存呈现（如果有）
            presentFromCache(m_currentAccountId);
            // 同步 worker
            if (m_syncWorker) {
                QMetaObject::invokeMethod(m_syncWorker, "watchAccount",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, m_currentAccountId));
            }
        }
    }
    if (m_sidebar) m_sidebar->rebuildTree(m_currentAccountId);
    setStatusText(QString("账号数：%1").arg(accs.size()));
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
    // 强制刷新：清缓存 + 触发后台重新同步
    m_sessionsCache.remove(m_currentAccountId);
    m_contactsCache.remove(m_currentAccountId);
    if (m_listPanel) m_listPanel->clearData(m_currentAccountId);

    if (m_syncWorker) {
        QMetaObject::invokeMethod(m_syncWorker, "syncAccount",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, m_currentAccountId));
    }
    setStatusText("已请求刷新当前账号…");
}

// ── sidebar 文件夹点击 → 中栏切页 + 触发加载 ──

void WeChatWidget::onSidebarChatFolderClicked(const QString& accId) {
    if (accId.isEmpty()) return;

    // 防抖：100ms 内重复点击同一 accId 同一文件夹 → 短路（防 sidebar 连点/抖动）
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool sameAccSameFolder = (accId == m_lastSidebarAccId)
        && (now - m_lastSidebarClickMs < 100);
    if (sameAccSameFolder && m_listPanel->isShowingChatList()) {
        // 已在该账号聊天页，跳过 watch + presentFromCache（省 SQLite + queued signal）
        return;
    }
    m_lastSidebarAccId = accId;
    m_lastSidebarClickMs = now;

    m_currentAccountId = accId;
    if (m_listPanel) {
        m_listPanel->setCurrentAccId(accId);
        m_listPanel->showChatList();
    }
    // 启动 watcher（首次点击某账号时也启动）
    if (m_syncWorker) {
        QMetaObject::invokeMethod(m_syncWorker, "watchAccount",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, accId));
    }
    // 优先从 CacheDb 读取（O(1)）；后台持续同步保证数据最新
    presentFromCache(accId);
}

void WeChatWidget::onSidebarContactFolderClicked(const QString& accId) {
    if (accId.isEmpty()) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool sameAccSameFolder = (accId == m_lastSidebarAccId)
        && (now - m_lastSidebarClickMs < 100);
    if (sameAccSameFolder && !m_listPanel->isShowingChatList()) {
        // 已在该账号联系人页，跳过 watch + presentFromCache
        return;
    }
    m_lastSidebarAccId = accId;
    m_lastSidebarClickMs = now;

    m_currentAccountId = accId;
    if (m_listPanel) {
        m_listPanel->setCurrentAccId(accId);
        m_listPanel->showContactList();
    }
    if (m_syncWorker) {
        QMetaObject::invokeMethod(m_syncWorker, "watchAccount",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, accId));
    }
    presentFromCache(accId);
}

// ── 中栏列表项点击 → 打开聊天 / 联系人详情 ──

void WeChatWidget::onListOpenChat(const QString& accId, const QString& talker) {
    if (talker.isEmpty() || accId.isEmpty()) return;
    m_currentAccountId = accId;

    // 计算 title（从 contacts 缓存找 display；群聊补成员数）
    QString title = talker;
    const auto& contacts = m_contactsCache.value(accId);
    for (const auto& v : contacts) {
        const auto c = v.toMap();
        if (c["userName"].toString() == talker) {
            title = c["display"].toString();
            break;
        }
    }
    if (title == talker && talker.endsWith(QLatin1String("@chatroom"))) {
        const auto members = CacheDb::loadChatRoomMembers(accId, talker);
        title += QString("（%1）").arg(members.size());
    }

    // 短路1：detailPanel 已经渲染过这个 talker 的气泡 → 仅切到 chatPage + 更新 title
    // 不调 loadMessages（避免和 watcher 抢 SQLite 锁），信任 detailPanel 内部短路
    if (m_currentTalker == talker && m_lastRenderedMsgCount > 0) {
        m_detailPanel->showChatHeader(title);
        return;
    }

    // 短路2：300ms 时间窗口（防 watcher 持续写入 + 连点导致反复重渲染）
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastOpenChatMs > 0 && now - m_lastOpenChatMs < 300) {
        m_detailPanel->showChatHeader(title);
        return;
    }
    m_lastOpenChatMs = now;

    // 直接从 CacheDb 读取消息（O(1)）；同步由后台 worker 异步进行
    const auto msgs = CacheDb::loadMessages(accId, talker, 0);
    if (msgs.isEmpty()) {
        // 缓存空：触发后台补全（可能是首次打开 / 未同步过的会话）
        if (m_syncWorker) {
            QMetaObject::invokeMethod(m_syncWorker, "syncAccount",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, accId));
        }
        m_detailPanel->showEmpty("该会话尚未同步到本地，等待后台首次同步完成…");
        m_currentTalker = talker;
        m_lastRenderedMsgCount = 0;
        return;
    }

    m_detailPanel->showChatHeader(title);
    m_detailPanel->renderMessages(msgs, talker);
    // m_currentTalker 由 renderMessages 内部维护（保证一致）
    m_lastRenderedMsgCount = msgs.size();
    setStatusText(QString("%1 · %2 条消息").arg(title).arg(msgs.size()));
}

void WeChatWidget::onListShowContact(const QString& accId, const QString& wxid) {
    if (accId.isEmpty() || wxid.isEmpty()) return;
    m_currentAccountId = accId;
    m_currentContact = wxid;

    // 1. 立即显示本地缓存（同步，零延迟）→ UI 立即响应
    bool found = false;
    const auto& list = m_contactsCache.value(accId);
    for (const auto& v : list) {
        const auto c = v.toMap();
        if (c["userName"].toString() == wxid) {
            m_detailPanel->showContact(c);                    // 内部短路
            found = true;
            break;
        }
    }
    if (!found) {
        m_detailPanel->showEmpty("联系人不存在（可能尚未同步）");
    }

    // 2. 异步加载详细信息（worker 线程，不阻塞 UI）
    //    完成时 contactDetailReady → onContactDetailReady → updateContactDetail
    //    只有还在显示同一联系人时才会更新 UI
    if (m_syncWorker) {
        QMetaObject::invokeMethod(m_syncWorker, "loadContactDetail",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, accId),
                                  Q_ARG(QString, wxid));
    }
}

void WeChatWidget::onContactDetailReady(const QString& accId, const QString& wxid,
                                         const QVariantMap& detail) {
    Q_UNUSED(accId);
    // 用户已经切到其他联系人 → 丢弃这个 detail（不更新 UI）
    if (m_currentContact != wxid) return;
    m_detailPanel->updateContactDetail(detail);
}

// ── SyncWorker 回调（主线程接收） ─────────────────────────────────────────────

void WeChatWidget::onSyncStarted(const QString& accId, const QString& stage) {
    Q_UNUSED(stage);
    auto* acc = WeChatAccountManager::instance().getById(accId);
    const QString name = acc ? acc->name : accId;
    setStatusText(QString("🔄 %1 后台同步中…").arg(name));
}

void WeChatWidget::onSyncProgress(const QString& accId, const QString& stage,
                                   int current, int total, const QString& msg) {
    Q_UNUSED(accId);
    const int pct = (total > 0) ? int(qreal(current) / total * 100.0) : 0;
    QString text;
    if (stage == "decrypt") {
        text = QString("🔓 正在解密数据库…");
    } else if (stage == "contacts") {
        text = QString("👥 同步联系人  %1  %2%").arg(msg).arg(pct);
    } else if (stage == "sessions") {
        text = QString("💬 同步会话    %1  %2%").arg(msg).arg(pct);
    } else if (stage == "messages") {
        text = QString("📨 同步消息    %1").arg(msg);
    } else {
        text = QString("[%1] %2 / %3  %4").arg(stage).arg(current).arg(total).arg(msg);
    }
    setStatusText(text);
}

void WeChatWidget::onSyncAccountDataReady(const QString& accId,
                                           const QVariantList& sessions,
                                           const QVariantList& contacts) {
    m_sessionsCache.insert(accId, sessions);
    m_contactsCache.insert(accId, contacts);
    if (m_listPanel) {
        m_listPanel->setSessions(accId, sessions);
        m_listPanel->setContacts(accId, contacts);
    }
}

void WeChatWidget::onSyncMessagesReady(const QString& accId, const QString& talker,
                                        const QString& title,
                                        const QList<QVariantMap>& messages) {
    Q_UNUSED(messages);
    // 仅当用户当前正在看这个会话 → 自动刷新详情
    if (m_currentAccountId != accId || m_currentTalker != talker) return;
    // 重读 CacheDb（避免数据竞态）
    const auto msgs = CacheDb::loadMessages(accId, talker, 0);
    if (msgs.isEmpty()) return;
    // 短路：消息数未变 → 不重建气泡（避免后台 syncCompleted 时 UI 反复重建）
    if (msgs.size() == m_lastRenderedMsgCount) return;
    m_detailPanel->showChatHeader(title);
    m_detailPanel->renderMessages(msgs, talker);
    m_lastRenderedMsgCount = msgs.size();
    setStatusText(QString("%1 · %2 条消息").arg(title).arg(msgs.size()));
}

void WeChatWidget::onSyncFinished(const QString& accId, qint64 elapsedMs) {
    auto* acc = WeChatAccountManager::instance().getById(accId);
    const QString name = acc ? acc->name : accId;
    m_lastSyncMs.insert(accId, QDateTime::currentMSecsSinceEpoch());
    const auto sCount = m_sessionsCache.value(accId).size();
    const auto cCount = m_contactsCache.value(accId).size();
    setStatusText(QString("✅ %1 同步完成（%2 ms · %3 会话 · %4 联系人）")
                      .arg(name)
                      .arg(elapsedMs)
                      .arg(sCount)
                      .arg(cCount));
}

void WeChatWidget::onSyncFailed(const QString& accId, const QString& reason) {
    auto* acc = WeChatAccountManager::instance().getById(accId);
    const QString name = acc ? acc->name : accId;
    setStatusText(QString("❌ %1 同步失败：%2").arg(name, reason));
}

// ── 缓存呈现 ────────────────────────────────────────────────────────────────

void WeChatWidget::presentFromCache(const QString& accId) {
    if (accId.isEmpty()) return;

    // 短路：重复点击同一 accId 且数据已加载 → 不重读 CacheDb（省 SQL + 不触发 listPanel setSessions）
    if (accId == m_currentAccountId &&
        m_sessionsCache.contains(accId) && m_contactsCache.contains(accId)) {
        return;
    }

    const auto sessions = CacheDb::loadSessions(accId, 0);
    const auto contacts = CacheDb::loadContacts(accId);
    m_sessionsCache.insert(accId, sessions);
    m_contactsCache.insert(accId, contacts);
    if (m_listPanel) {
        m_listPanel->setSessions(accId, sessions);   // 内部指纹短路：相同数据不重建 list
        m_listPanel->setContacts(accId, contacts);
    }
    if (!sessions.isEmpty() || !contacts.isEmpty()) {
        setStatusText(QString("%1 · 缓存命中：%2 会话 / %3 联系人")
                          .arg(accId)
                          .arg(sessions.size())
                          .arg(contacts.size()));
    }
}

bool WeChatWidget::hasCached(const QString& accId) const {
    return m_sessionsCache.contains(accId) && m_contactsCache.contains(accId);
}