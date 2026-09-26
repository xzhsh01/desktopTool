#pragma once

#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

class QLabel;
class QStackedWidget;
class QSplitter;
class QThread;

class WeChatSidebar;
class WeChatDetailPanel;
class WeChatListPanel;
class WeChatWorker;

/**
 * WeChatWidget: 微信主界面（协调者）
 *
 * 三栏布局：
 *   ┌────────────┬──────────────────┬────────────────────────────┐
 *   │ Sidebar    │ ListPanel        │ DetailPanel                │
 *   │ 账号+文件夹│ 会话/联系人列表   │ empty / chat / contact     │
 *   │ （左栏）   │ （中栏）          │ （右栏）                    │
 *   ├────────────┴──────────────────┴────────────────────────────┤
 *   │ 底部状态栏（账号加载情况 / 数据库解密）                       │
 *   └────────────────────────────────────────────────────────────┘
 *
 * 信号流：
 *   sidebar 文件夹点击 → 协调者切换 listPanel 页 + 触发数据加载
 *   listPanel 项点击   → 协调者打开聊天 / 联系人详情
 *   worker 加载完成    → 协调者缓存数据 + 注入 listPanel
 *
 * 无账号时整页切换为 m_emptyPage（居中卡片 + 添加按钮）。
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

    // worker 完成回调（在主线程接收）
    void onAccountLoaded(const QString& accId,
                         const QVariantList& sessions,
                         const QVariantList& contacts);
    void onAccountFailed(const QString& accId, const QString& reason);
    void onMessagesLoaded(const QString& accId,
                          const QString& talker,
                          const QString& title,
                          const QList<QVariantMap>& messages);
    void onMessagesFailed(const QString& accId,
                          const QString& talker,
                          const QString& reason);

private:
    void buildUi();
    void updateEmptyState();                // 切换 m_mainStack
    void setCurrentAccount(const QString& accId);   // 切换当前账号 + 同步中栏

    // 启动后台 worker 线程（数据加载专用）
    void startWorker();
    // 停止后台 worker 线程（析构时调用）
    void stopWorker();
    // 数据加载（按账号缓存；命中缓存直接返回，未命中交给 worker）
    bool loadAccountData(const QString& accId);
    void setStatus(const QString& text);

    // ── 控件 ──
    QStackedWidget*   m_mainStack   = nullptr;     // 0 空账号 / 1 工作
    QWidget*          m_emptyPage   = nullptr;     // 居中引导
    QWidget*          m_workPage    = nullptr;
    QSplitter*        m_splitter    = nullptr;
    WeChatSidebar*    m_sidebar     = nullptr;
    WeChatListPanel*  m_listPanel   = nullptr;
    WeChatDetailPanel* m_detailPanel = nullptr;

    // ── 后台线程（数据加载专用） ──
    QThread*      m_loadThread = nullptr;
    WeChatWorker* m_loadWorker = nullptr;

    // ── 缓存 ──
    QHash<QString, QVariantList> m_sessionsCache;
    QHash<QString, QVariantList> m_contactsCache;

    QString m_currentAccountId;
    QString m_currentTalker;
};