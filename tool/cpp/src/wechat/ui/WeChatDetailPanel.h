#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QVariantMap>
#include <QWidget>

class QLabel;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

/**
 * WeChatDetailPanel: 微信详情面板（右栏）
 *
 * 三页 QStackedWidget 切换：
 * - emptyPage：未选中聊天 / 联系人时的提示
 * - chatPage：标题 + 消息气泡流（懒加载，由协调者触发填充）
 * - contactPage：联系人 / 群详情
 *
 * 气泡渲染、联系人详情卡片构造、日期分隔等纯 UI 逻辑都在本面板内；
 * 数据库访问（loadMessages / displayName）由协调者 WeChatWidget 调度。
 */
class WeChatDetailPanel : public QWidget {
    Q_OBJECT

public:
    explicit WeChatDetailPanel(QWidget* parent = nullptr);

    // ── 详情切换 ──
    void showEmpty(const QString& hint = QString());
    void showChatHeader(const QString& title);                   // 仅切到聊天页 + 改标题
    void showContact(const QVariantMap& contact);                 // 联系人 / 群详情

    // 异步详细信息到达 → 增量更新当前联系人详情（不重渲染其他 widget）
    void updateContactDetail(const QVariantMap& detail);

    // 在聊天页里渲染一组消息（协调者把 db 拉到的原始数据转成 vmap 后传入）
    void renderMessages(const QList<QVariantMap>& msgs,
                        const QString& currentTalker);

private:
    QWidget* makeDetailPanel();                            // 构建三页
    QWidget* makeBubble(const QVariantMap& msg);           // 单条消息气泡
    QWidget* makeDateSeparator(const QDateTime& t);        // 日期分隔
    QLabel*  makeAvatar(const QString& name, const QString& key, int size);

    // 控件
    QStackedWidget* m_detail       = nullptr;
    QWidget*        m_emptyPage    = nullptr;
    QLabel*         m_emptyHint    = nullptr;

    QWidget*        m_chatPage     = nullptr;
    QLabel*         m_chatTitle    = nullptr;
    QScrollArea*    m_msgScroll    = nullptr;
    QWidget*        m_msgContainer = nullptr;
    QVBoxLayout*    m_msgLayout    = nullptr;

    QWidget*        m_contactPage  = nullptr;

    QString m_currentTalker;                                // 用于渲染头像/群名
    QString m_currentShownContact;                          // showContact 短路用（按 wxid）
    int     m_renderedMsgCount = 0;                         // renderMessages 内部短路用
};