#pragma once

#include <QString>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

/**
 * WeChatSidebar: 微信侧边栏面板（左栏）
 *
 * 职责（三栏布局下）：
 * - 顶部按钮行：[+ 添加账号] [↻ 刷新]
 * - QTreeWidget：账号根 + 两个文件夹（💬 聊天 / 👥 联系人）
 * - 文件夹被点击时发出信号，由协调者在中间列表面板中展示列表
 * - 不再展开叶子（聊天 / 联系人列表移到中间面板）
 *
 * 数据加载（数据库解密、会话 / 联系人列表）由协调者 WeChatWidget 监听信号后
 * 执行；本面板仅承担导航与触发。
 */
class WeChatSidebar : public QWidget {
    Q_OBJECT

public:
    explicit WeChatSidebar(QWidget* parent = nullptr);

    QString currentAccountId() const;
    void    selectAccount(const QString& accId);    // 选中指定账号并展开

    // 重新构建账号树（账号列表变化时由协调者调用）
    void rebuildTree(const QString& selectAccId = QString());

signals:
    void addAccountRequested();
    void editAccountRequested(const QString& accId);
    void deleteAccountRequested(const QString& accId);
    void refreshRequested();

    // 文件夹被点击：协调者据此在中间面板显示对应列表
    void chatFolderClicked(const QString& accId);
    void contactFolderClicked(const QString& accId);

private:
    // ── 节点类型 ──
    enum NodeType {
        NodeAccountRoot = 1,
        NodeChatFolder,
        NodeContactFolder,
    };
    constexpr static int NodeTypeRole = Qt::UserRole + 2;

    void buildUi();
    QTreeWidgetItem* makeAccountItem(const QString& accId, const QString& name) const;
    QTreeWidgetItem* makeFolderItem(const QString& accId,
                                    const QString& title, NodeType type) const;
    QTreeWidgetItem* findAccountItem(const QString& accId) const;

    // 槽
    void onTreeItemClicked(QTreeWidgetItem* item, int col);
    void onContextMenu(const QPoint& pos);
    void onAddClicked();
    void onRefreshClicked();

    // ── 控件 ──
    QTreeWidget* m_tree = nullptr;
};