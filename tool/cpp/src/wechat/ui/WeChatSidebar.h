#pragma once

#include <QHash>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

/**
 * WeChatSidebar: 微信侧边栏面板（左栏）
 *
 * 树结构（简化版）：
 *   📁 账号A   (root, 不可点击 — 充当容器)
 *     👥 联系人  (group header, 可点击 → contactGroupClicked)
 *     💬 聊天    (group header, 可点击 → chatGroupClicked)
 *   📁 账号B
 *     👥 联系人
 *     💬 聊天
 *
 * 设计：账号根用于右键"编辑/删除"和承载分组；具体联系人 / 会话列表
 *       不在侧边栏展开为子节点，而是统一在中栏（ListPanel）展示，
 *       这样侧边栏更紧凑，避免几百条会话撑爆左树。
 *
 * 视觉（参考邮件样式）：
 *   - 三个状态：hover / pressed / selected，都给明确反馈
 *   - "💬 聊天" 右侧带未读徽标（HTML 内嵌红圆 + 数字）
 *   - "👥 联系人" 右侧可显示最近联系人时间（如 "12:34"）
 *
 * 信号：
 *   chatGroupClicked(accId)        点击 "聊天" 分组
 *   contactGroupClicked(accId)     点击 "联系人" 分组
 */
class WeChatSidebar : public QWidget {
    Q_OBJECT

public:
    explicit WeChatSidebar(QWidget* parent = nullptr);

    QString currentAccountId() const;
    void    selectAccount(const QString& accId);    // 选中指定账号并展开

    // 重新构建账号树骨架（账号列表变化时由协调者调用）
    void rebuildTree(const QString& selectAccId = QString());

    // 把指定账号的 sessions/contacts 灌入 — 用于更新分组计数与徽标
    void setLeafData(const QString& accId,
                     const QVariantList& sessions,
                     const QVariantList& contacts);

    // 流式增量追加：worker 每 N 条回调一次；经 coalescing 定时器合并刷新徽标
    void appendContactsBatch(const QString& accId, const QVariantList& batch);
    void appendSessionsBatch(const QString& accId, const QVariantList& batch);

    // 清掉某账号的缓存（强制下次 setLeafData 重建计数）
    void clearLeafData(const QString& accId);

signals:
    void addAccountRequested();
    void editAccountRequested(const QString& accId);
    void deleteAccountRequested(const QString& accId);
    void refreshRequested();

    // 分组被点击（账号 id 透出，便于协调者切换 ListPanel 数据源）
    void chatGroupClicked(const QString& accId);
    void contactGroupClicked(const QString& accId);

private:
    // ── 节点类型 ──
    enum NodeType {
        NodeAccountRoot   = 1,
        NodeContactGroup,           // 联系人分组头（可点击）
        NodeChatGroup,              // 聊天分组头（可点击）
    };
    constexpr static int NodeTypeRole = Qt::UserRole + 2;

    void buildUi();
    QTreeWidgetItem* makeAccountItem(const QString& accId, const QString& name) const;
    QTreeWidgetItem* makeGroupItem(NodeType type) const;
    QTreeWidgetItem* findAccountItem(const QString& accId) const;

    // 刷新某账号分组的标题（含计数 + 未读徽标 + 最近时间）；fingerprint 短路
    void refreshGroupLabelsForAccount(const QString& accId);

    // 工具：根据当前 sessions/contacts 数据生成 "💬 聊天" / "👥 联系人" 富文本标签
    QString buildChatLabel(const QVariantList& sessions) const;
    QString buildContactLabel(const QVariantList& contacts) const;

    // 工具：把整数秒时间戳格式化为 "12:34" 或 "昨天 10:21" / "2025-09-21"
    static QString formatRelativeTime(qint64 secsSinceEpoch);
    // 工具：把整数未读数格式化为 "12" 或 "99+"
    static QString formatBadge(int n);

    // 槽
    void onTreeItemClicked(QTreeWidgetItem* item, int col);
    void onContextMenu(const QPoint& pos);
    void onAddClicked();
    void onRefreshClicked();

    // ── 控件 ──
    QTreeWidget* m_tree = nullptr;
    // 每个账号的会话/联系人数据缓存（用于生成徽标）
    QHash<QString, QVariantList> m_sessionsByAcc;
    QHash<QString, QVariantList> m_contactsByAcc;
    // 每个账号的"内容指纹"：用于判断是否需要刷新徽标
    QHash<QString, QString> m_contactsLabelFp;
    QHash<QString, QString> m_sessionsLabelFp;

    // Coalescing 定时器：流式 partial 期间合并刷新徽标
    QHash<QString, QTimer*> m_rebuildTimerByAcc;
    void scheduleRefresh(const QString& accId, int delayMs = 150);
};