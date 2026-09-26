#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

class QPushButton;
class QTableWidget;
#include "mail/MailStore.h"

/**
 * MailListPanel: 邮件列表面板（中栏）
 *
 * 职责：
 * - 顶部按钮栏：写邮件 / 删除 / 刷新（点击经信号透出，由 MailWidget 执行）
 * - 单列块状邮件表格（MailListDelegate 自绘 发件人/时间/主题/摘要 + 复选框）
 * - 列表数据构建：搜索过滤、未读置顶 + 按日期分组（今天/昨天/本周/更早）
 *   放后台线程，主线程批量填充；草稿文件夹本地渲染
 * - 勾选状态管理（批量删除用）
 *
 * 数据：MailStore 非线程安全，仅主线程取快照；行数据为纯值类型跨线程传递。
 */
class MailListPanel : public QWidget {
    Q_OBJECT

public:
    explicit MailListPanel(QWidget* parent = nullptr);

    // 重建列表（accId/folder 任一为空则清空）
    void refreshMessages(const QString& accountId, const QString& folder,
                         const QString& keyword);

    QString selectedKey() const;        // 当前行 key（邮件 id 或 "draft:<id>"）
    QString selectedMessageId() const;  // 当前行邮件 id（草稿/无选中返回空）
    QString selectedDraftId() const;    // 当前行草稿 id（邮件/无选中返回空）
    bool    hasSelection() const;
    void    setDeleteEnabled(bool on);

    // 右键菜单辅助
    int     rowAt(const QPoint& pos) const;
    bool    isGroupRow(int row) const;
    QString keyAt(int row) const;
    bool    isChecked(int row) const;
    void    setChecked(int row, bool on);
    void    setCurrentRow(int row);

    // 收集勾选项（批量删除）
    void collectChecked(QStringList& mailIds, QStringList& draftIds) const;

signals:
    void newMailRequested();
    void deleteRequested();
    void syncRequested();

    void selectionChanged();                     // 选中行变化
    void itemDoubleClicked(const QString& key);  // 双击（key 含 draft: 前缀）
    void contextMenuRequested(const QPoint& pos);// 右键（透传，由 MailWidget 分发）
    void rowClicked();                           // 左键点击行（cellClicked）

private:
    // 列表行数据：后台线程构建（过滤/分组/格式化），主线程填充表格
    struct ListRow {
        QString key;      // 邮件 id（UserRole）
        QString kind;     // "mail" / "group"（UserRole+1）
        QString from;     // 发件人（UserRole+2）
        QString time;     // 显示时间（UserRole+3）
        QString subject;  // 主题（UserRole+4）
        bool    unread = false;   // 未读（UserRole+6，delegate 加粗+高亮发件人）
    };

    void fillTable(const QList<ListRow>& rows, const QString& keepKey);
    void fillDrafts(const QString& accountId, const QString& keyword,
                    const QString& keepKey);
    // 通用邮件行渲染（过滤/未读置顶/分组/格式化后台、填充主线程）
    void renderMailRows(const QList<MailStore::Message>& msgs,
                        const QString& keyword, const QString& keepKey);

    // ── 无感刷新辅助 ──
    // 行内容指纹：与上次一致则跳过重建（勾选/滚动/选中天然保留）
    static QByteArray rowsFingerprint(const QList<ListRow>& rows);
    QSet<QString> checkedKeysSnapshot() const;   // 重建前快照勾选集合
    QString topVisibleKey() const;               // 顶部可见行 key（滚动恢复锚点）
    void restoreScroll(const QString& topKey, int fallbackPos);

    QPushButton*  m_deleteBtn = nullptr;
    QTableWidget* m_table = nullptr;
    quint64       m_listGen = 0;   // 列表加载代次（丢弃过期后台结果）

    // 无感刷新状态：上次填充指纹 + 上次视图参数（同视图刷新不预清空列表）
    QByteArray m_lastFingerprint;
    QString    m_lastAcc, m_lastFolder, m_lastKw;
};
