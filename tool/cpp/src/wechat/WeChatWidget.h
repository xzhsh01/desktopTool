#pragma once

#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

class QLabel;
class QStackedWidget;
class QSplitter;
class QThread;
class QTimer;

class WeChatSidebar;
class WeChatDetailPanel;
class WeChatListPanel;
class WeChatSyncWorker;

/**
 * WeChatWidget: 微信主界面（协调者）
 *
 * 三栏布局：
 *   ┌────────────┬──────────────────┬────────────────────────────┐
 *   │ Sidebar    │ ListPanel        │ DetailPanel                │
 *   │ 账号+文件夹│ 会话/联系人列表   │ empty / chat / contact     │
 *   │ （左栏）   │ （中栏）          │ （右栏）                    │
 *   ├────────────┴──────────────────┴────────────────────────────┤
 *   │ 底部状态栏（实时同步进度：账号 · 阶段 · current/total · 消息） │
 *   └────────────────────────────────────────────────────────────┘
 *
 * 数据流（新架构）：
 *
 *   SyncWorker 线程                                  UI 线程
 *   ┌─────────────────┐                              ┌────────────┐
 *   │ QFileSystemWatcher                                │
 *   │       ↓                                           │
 *   │ WeChatDb::ensureDecrypted()                       │
 *   │       ↓                                           │
 *   │ loadContacts / loadSessions / loadMessages        │
 *   │       ↓ 流式 emit                                  │
 *   │ CacheDb::replaceContacts/Sessions/Messages        │
 *   │       ↓ emit syncAccountDataReady                  │
 *   └─────────────────┘                              ─→ 读 CacheDb → 渲染
 *
 *   - UI 只读 CacheDb（O(1)），永远不阻塞主线程
 *   - 后台同步增量化（按 mtime/size 识别），重复点击零延迟
 */
class WeChatWidget : public QWidget {
    Q_OBJECT

public:
    explicit WeChatWidget(QWidget* parent = nullptr);
    ~WeChatWidget() override;

    // 外部入口（应用管理 / MainWindow 调用）
    void openConfig(const QString& editId = QString());
    void selectAccount(const QString& accountId);

private slots:
    // 监听账号变化
    void onAccountsChanged();

    // sidebar 文件夹点击 → 中栏切换 + 触发加载
    void onSidebarChatFolderClicked(const QString& accId);
    void onSidebarContactFolderClicked(const QString& accId);

    // listPanel 项点击 → 打开聊天 / 联系人详情
    void onListOpenChat(const QString& accId, const QString& talker);
    void onListShowContact(const QString& accId, const QString& wxid);

    // 侧边栏账号操作
    void onAddAccount();
    void onEditAccount(const QString& accId);
    void onDeleteAccount(const QString& accId);
    void onRefreshCurrent();

    // SyncWorker 回调（在主线程接收）
    void onSyncStarted(const QString& accId, const QString& stage);
    void onSyncProgress(const QString& accId, const QString& stage,
                        int current, int total, const QString& msg);
    void onSyncAccountDataReady(const QString& accId,
                                const QVariantList& sessions,
                                const QVariantList& contacts);
    void onSyncMessagesReady(const QString& accId, const QString& talker,
                             const QString& title,
                             const QList<QVariantMap>& messages);
    void onSyncFinished(const QString& accId, qint64 elapsedMs);
    void onSyncFailed(const QString& accId, const QString& reason);

    // 状态栏节流刷新（同步进度事件频率很高，合并刷新）
    void onStatusTick();

private:
    void buildUi();
    void updateEmptyState();                // 切换 m_mainStack
    void setCurrentAccount(const QString& accId);   // 切换当前账号 + 同步中栏

    // 启动后台 sync worker 线程
    void startSyncWorker();
    // 停止后台 sync worker 线程（析构时调用）
    void stopSyncWorker();

    // 同步显示某账号（从 CacheDb 取数据注入 UI）
    void presentFromCache(const QString& accId);
    // 缓存查询：判断某账号是否已有同步好的数据
    bool hasCached(const QString& accId) const;
    // 状态栏文本（节流：只显示"最后一次 updateStatusBar 调用"）
    void updateStatusBar();
    void setStatusText(const QString& text);

    // ── 控件 ──
    QStackedWidget*   m_mainStack   = nullptr;     // 0 空账号 / 1 工作
    QWidget*          m_emptyPage   = nullptr;     // 居中引导
    QWidget*          m_workPage    = nullptr;
    QSplitter*        m_splitter    = nullptr;
    WeChatSidebar*    m_sidebar     = nullptr;
    WeChatListPanel*  m_listPanel   = nullptr;
    WeChatDetailPanel* m_detailPanel = nullptr;
    QLabel*           m_statusLabel = nullptr;     // 底部状态栏

    // ── 后台同步线程 ──
    QThread*            m_syncThread = nullptr;
    WeChatSyncWorker*   m_syncWorker = nullptr;

    // ── UI 缓存（已渲染的元数据，避免重复注入 listPanel） ──
    QHash<QString, QVariantList> m_sessionsCache;     // accId -> sessions
    QHash<QString, QVariantList> m_contactsCache;     // accId -> contacts
    QHash<QString, qint64>       m_lastSyncMs;        // accId -> 上次完成时间戳

    QString m_currentAccountId;
    QString m_currentTalker;

    // ── 状态栏节流 ──
    QTimer* m_statusTickTimer = nullptr;
    QString m_pendingStatusText;
};