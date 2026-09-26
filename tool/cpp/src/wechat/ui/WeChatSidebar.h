#pragma once

#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

/**
 * WeChatSidebar: 微信侧边栏面板（左栏）
 *
 * 职责：
 * - 顶部按钮行：[+ 添加账号 (stretch=1)] [↻ 刷新]
 * - 搜索框（按昵称/备注过滤聊天与联系人）
 * - QTreeWidget：账号 + 聊天 / 联系人 两级
 * - 右键菜单：账号行 = 编辑/删除/添加；任意位置 = 添加
 * - 数据懒加载（聊天 / 联系人按需填充，搜索时自动展开）
 *
 * 业务逻辑（数据库读写、加载）由协调者 WeChatWidget 监听信号后执行；
 * 本面板仅承担 UI 与状态管理。
 */
class WeChatSidebar : public QWidget {
    Q_OBJECT

public:
    explicit WeChatSidebar(QWidget* parent = nullptr);

    QString currentAccountId() const;
    void    selectAccount(const QString& accId);    // 选中指定账号并定位展开

    // 数据注入（由协调者 WeChatWidget 提供，懒加载缓存）
    void setSessions(const QString& accId, const QVariantList& list);
    void setContacts(const QString& accId, const QVariantList& list);
    bool hasData(const QString& accId) const;
    void clearData(const QString& accId);             // 强制重新加载

    QString searchText() const;

    // 重新构建账号树（账号列表变化时由协调者调用）
    void rebuildTree(const QString& selectAccId = QString());

signals:
    void addAccountRequested();
    void editAccountRequested(const QString& accId);
    void deleteAccountRequested(const QString& accId);
    void refreshRequested();

    // 协调者收到此信号后真正加载会话/联系人数据并 setSessions
    void loadSessionsRequested(const QString& accId);
    void loadContactsRequested(const QString& accId);

    // 选中叶子：协调者据此打开聊天 / 联系人详情
    void openChatRequested(const QString& accId, const QString& talker);
    void showContactRequested(const QString& accId, const QString& wxid);

private:
    // ── 节点类型 ──
    enum NodeType {
        NodeAccountRoot = 1,
        NodeChatFolder,
        NodeContactFolder,
        NodeChatLeaf,
        NodeContactLeaf,
    };
    constexpr static int NodeTypeRole = Qt::UserRole + 2;

    void buildUi();
    QTreeWidgetItem* makeAccountItem(const QString& accId, const QString& name) const;
    QTreeWidgetItem* makeFolderItem(const QString& accId,
                                    const QString& title, NodeType type) const;

    void fillChatLeaves(QTreeWidgetItem* folder, const QString& accId);
    void fillContactLeaves(QTreeWidgetItem* folder, const QString& accId);
    void updateFolderCount(const QString& accId);
    void clearFolderChildren(QTreeWidgetItem* folder);
    QTreeWidgetItem* findAccountItem(const QString& accId) const;

    // 槽
    void onTreeItemClicked(QTreeWidgetItem* item, int col);
    void onTreeItemExpanded(QTreeWidgetItem* item);
    void onSearchChanged(const QString& text);
    void onContextMenu(const QPoint& pos);
    void onAddClicked();
    void onRefreshClicked();

    // ── 控件 ──
    QLineEdit*   m_searchEdit = nullptr;
    QTreeWidget* m_tree       = nullptr;

    // ── 状态 ──
    QHash<QString, QVariantList> m_sessionsCache;   // accId → ChatSession
    QHash<QString, QVariantList> m_contactsCache;   // accId → Contact
};