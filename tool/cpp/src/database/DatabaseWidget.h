#pragma once

#include <QWidget>
#include <QList>

class QTabWidget;
class DbSessionPane;

/**
* DatabaseWidget: 数据库客户端容器（多连接 tab）
* 每个 tab = 一个 DbSessionPane（独立 DatabaseClient + 对象树 + SQL 编辑器），
* 互不影响。后台同时保持连接，切换 tab 不打断查询。
*
* 顶层只负责：
*  - tab 增删/切换（含空状态欢迎页）
*  - "+ 新连接" 按钮（弹出已配置连接列表）
*  - 转发 connectTo(id) 到已存在 session 或新建 session
*/
class DatabaseWidget : public QWidget {
    Q_OBJECT

public:
    explicit DatabaseWidget(QWidget* parent = nullptr);

    // 供外部调用（Dashboard 快速连接）：
    //  - 已存在该 connId 的 tab → 切到该 tab
    //  - 否则 → 新建 tab 并发起连接
    void connectTo(const QString& connectionId);

private:
    void setupUI();
    void onNewConnection();              // "+ 连接"按钮：弹出已配置连接菜单
    void onSessionConnecting(const QString& connId, const QString& displayName);
    void onSessionConnected(const QString& connId, const QString& serverVersion);
    void onSessionDisconnected(const QString& connId);
    void onTabCloseRequested(int index); // 关闭 tab：弹确认 → 断开连接 → 销毁

    DbSessionPane* findSession(const QString& connId) const;
    QWidget* buildWelcomeWidget();       // 构造空状态欢迎页

    QTabWidget* m_sessionTabs = nullptr;
    QList<DbSessionPane*> m_sessions;  // 与 m_sessionTabs 同序
    int m_welcomeIndex = -1;            // 欢迎页在 m_sessionTabs 中的下标，-1 表示无
};
