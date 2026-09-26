#pragma once

#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

class QLabel;
class QStackedWidget;
class QSplitter;

class WeChatSidebar;
class WeChatDetailPanel;

/**
 * WeChatWidget: 微信主界面（协调者）
 *
 * 布局（参考邮箱 MailWidget 架构）：
 *   ┌────────────────────────────────────────────┐
 *   │ 顶部：标题"微信"                  [状态]  │   工具栏
 *   ├────────────────────────────────────────────┤
 *   │                                            │
 *   │    [WeChatSidebar]  │  [WeChatDetailPanel] │
 *   │    （搜索 + 树）    │  （empty/chat/contact）│   splitter 二栏
 *   │                                            │
 *   ├────────────────────────────────────────────┤
 *   │ 底部状态栏（账号加载情况 / 数据库解密）      │
 *   └────────────────────────────────────────────┘
 *
 *  - 无账号时整页切换为 m_emptyPage（居中卡片 + 添加按钮）
 *  - 业务逻辑（数据库加载、密钥解密）在本类内完成；
 *    UI / 状态在两个独立面板中。
 */
class WeChatWidget : public QWidget {
    Q_OBJECT

public:
    explicit WeChatWidget(QWidget* parent = nullptr);

    // 外部入口（应用管理 / MainWindow 调用）
    void openConfig(const QString& editId = QString());
    void selectAccount(const QString& accountId);

private slots:
    // 监听账号变化
    void onAccountsChanged();
    // 侧边栏信号
    void onSidebarLoadSessions(const QString& accId);
    void onSidebarLoadContacts(const QString& accId);
    void onSidebarOpenChat(const QString& accId, const QString& talker);
    void onSidebarShowContact(const QString& accId, const QString& wxid);
    // 侧边栏账号操作
    void onAddAccount();
    void onEditAccount(const QString& accId);
    void onDeleteAccount(const QString& accId);
    void onRefreshCurrent();

private:
    void buildUi();
    void updateEmptyState();                // 切换 m_mainStack

    // 数据加载（按账号缓存）
    bool loadAccountData(const QString& accId);
    void setStatus(const QString& text);

    // ── 控件 ──
    QStackedWidget*   m_mainStack   = nullptr;     // 0 空账号 / 1 工作
    QWidget*          m_emptyPage   = nullptr;     // 居中引导
    QWidget*          m_workPage    = nullptr;
    QSplitter*        m_splitter    = nullptr;
    WeChatSidebar*    m_sidebar     = nullptr;
    WeChatDetailPanel* m_detailPanel = nullptr;
    QLabel*           m_statusLabel = nullptr;

    // ── 缓存 ──
    QHash<QString, QVariantList> m_sessionsCache;
    QHash<QString, QVariantList> m_contactsCache;

    QString m_currentAccountId;
    QString m_currentTalker;
};