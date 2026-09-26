#pragma once

#include <QHash>
#include <QString>
#include <QVariantList>
#include <QWidget>

class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QLineEdit;

/**
 * WeChatListPanel: 微信中间列表面板（中栏）
 *
 * 两页 QStackedWidget 切换：
 * - 0：会话列表（来自 sidebar "💬 聊天" 文件夹点击）
 * - 1：联系人列表（来自 sidebar "👥 联系人" 文件夹点击）
 *
 * 职责：
 * - 顶部：标题 + 计数 + 搜索框（按昵称/标题过滤）
 * - 列表：QListWidget，项 widget 显示头像 + 主名 + 副标题（聊天页 lastMsg / 联系人群标签）
 * - 点击列表项发出信号（accId, talker/wxid），由协调者 WeChatWidget 打开聊天 / 详情
 *
 * 数据由协调者通过 setSessions / setContacts 注入（与 sidebar 折叠状态解耦）。
 */
class WeChatListPanel : public QWidget {
    Q_OBJECT

public:
    explicit WeChatListPanel(QWidget* parent = nullptr);

    // 数据注入（按账号缓存；命中时只刷新当前可见那一页）
    void setSessions(const QString& accId, const QVariantList& list);
    void setContacts(const QString& accId, const QVariantList& list);
    void clearData(const QString& accId);

    // 切换显示哪一页
    void showChatList();
    void showContactList();

    // 设置当前面板对应的账号（数据展示的目标账号）
    void setCurrentAccId(const QString& accId);
    QString currentAccId() const { return m_currentAccId; }

    // 当前正在显示哪一页（用于协调者判断）
    bool isShowingChatList() const;

    // 当前过滤文本（保留供外部使用）
    QString searchText() const;

signals:
    // 列表项被点击：协调者据此打开聊天 / 联系人详情
    void chatItemClicked(const QString& accId, const QString& talker);
    void contactItemClicked(const QString& accId, const QString& wxid);

private:
    void buildUi();
    void rebuildChatList();
    void rebuildContactList();
    void updateTitle();

    QListWidgetItem* makeChatItem(const QVariantMap& s);
    QListWidgetItem* makeContactItem(const QVariantMap& c);

private slots:
    void onChatItemActivated(QListWidgetItem* item);
    void onContactItemActivated(QListWidgetItem* item);
    void onSearchChanged(const QString& text);
    void onCurrentRowChanged();

private:
    QStackedWidget* m_stack       = nullptr;
    QListWidget*    m_chatList    = nullptr;
    QListWidget*    m_contactList = nullptr;
    QLineEdit*      m_searchEdit  = nullptr;
    QWidget*        m_titleBar    = nullptr;   // 标题 + 计数（两页共用一份视觉）

    // 缓存：accId → 数据
    QHash<QString, QVariantList> m_sessionsCache;
    QHash<QString, QVariantList> m_contactsCache;

    QString m_currentAccId;        // 当前面板对应的账号（决定数据源）
    QString m_currentTalker;       // 当前聊天页选中项的 talker（高亮 / 协调者用）
};