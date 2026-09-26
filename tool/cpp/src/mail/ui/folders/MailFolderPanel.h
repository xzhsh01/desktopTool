#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

#include "mail/ImapClient.h"

class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

/**
 * MailFolderPanel: 邮件文件夹面板（左栏）
 *
 * 职责：
 * - 顶部搜索框（过滤邮件列表，textChanged 经 searchChanged 信号透出）
 * - 账号 + 文件夹树（顶级 = 邮箱账号，子级 = 文件夹，支持 IMAP 多级路径）
 * - 未读数徽标、同名文件夹去重、容器节点未读聚合
 * - 右键菜单：添加 / 编辑 / 删除 / 设为默认 / 刷新（动作经信号透出，由 MailWidget 执行）
 *
 * 数据：本地固定文件夹（INBOX/Sent/Drafts）+ IMAP LIST 缓存（setRemoteFolders 注入）
 */
class MailFolderPanel : public QWidget {
    Q_OBJECT

public:
    explicit MailFolderPanel(QWidget* parent = nullptr);

    QString currentAccountId() const;   // 当前选中节点所属账号（顶级/子级均可）
    QString currentFolder() const;      // 当前选中文件夹 key（顶级账号节点返回空）
    QString searchText() const;         // 搜索框关键词（trimmed）
    void    selectAccount(const QString& id);   // 选中指定账号顶级节点

    void    rebuild();                  // 全量重建树（保留并恢复选择）
    void    setRemoteFolders(const QString& accountId,
                             const QList<ImapClient::Folder>& folders);
    void    removeRemoteFolders(const QString& accountId);
    bool    hasRemoteFolders(const QString& accountId) const;
    QList<ImapClient::Folder> remoteFolders(const QString& accountId) const;

    // 文件夹 key（本地固定 key 或 IMAP 原始名）→ 弹窗显示名
    static QString displayNameForKey(const QString& key);

signals:
    void selectionChanged();            // 树选择变化（账号或文件夹）
    void searchChanged();               // 搜索框文本变化
    void newMailRequested();            // "写邮件"按钮（顶部搜索框上方）
    void refreshRequested();            // "刷新"图标按钮（写邮件按钮右侧）

    void addAccountRequested();
    void editAccountRequested(const QString& accountId);
    void deleteAccountRequested(const QString& accountId);
    void setDefaultRequested(const QString& accountId);
    // 刷新：folderKey 非空 = 仅同步该文件夹；空 = 账号全量刷新
    void refreshFolderRequested(const QString& accountId, const QString& folderKey);
    void refreshAccountRequested(const QString& accountId);

private:
    void onContextMenu(const QPoint& pos);

    QTreeWidget* m_tree = nullptr;
    QLineEdit*   m_searchEdit = nullptr;
    QPushButton* m_newMailBtn = nullptr;        // "写邮件"按钮（搜索框上方）
    QPushButton* m_refreshBtn = nullptr;        // "刷新"图标按钮（写邮件按钮右侧）

    // IMAP 真实文件夹缓存：accountId -> 服务器返回的文件夹列表
    QHash<QString, QList<ImapClient::Folder>> m_remoteFolders;
};
