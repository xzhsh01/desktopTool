#pragma once

#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "mail/MailStore.h"

class AttachmentPreviewPane;

class QFormLayout;
class QFrame;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;
class QTimer;
class QPropertyAnimation;
class QGraphicsOpacityEffect;
class QResizeEvent;
class MailPreviewBrowser;

/**
 * MailContentPanel: 三段式邮件内容面板（toolbar / header / attach / body）
 */
class MailContentPanel : public QWidget {
    Q_OBJECT

public:
    explicit MailContentPanel(QWidget* parent = nullptr);

    void showMessage(const QString& msgId);
    void showDraft(const QString& draftId);   // 在预览区显示草稿正文
    void clearPreview();
    void refresh();
    void refreshRawIfCurrent(const QString& msgId);

    QString currentMsgId() const { return m_currentMsgId; }
    bool    isShowingRaw() const { return m_showingRaw; }
    void    setActionsEnabled(bool on);

    static QString attachmentSavedPath(const QString& msgId,
                                      const MailStore::Attachment& a);

    bool isAttachmentDownloading(const QString& key) const;
    void setAttachmentDownloading(const QString& key, bool on);
    void setAttachmentProgress(const QString& key, qint64 received, qint64 expected);
    void clearAttachmentProgress(const QString& key);
    // 写盘后调用：ok=true → 行变"已下载"并显示打开/定位按钮；
    // ok=false → 行显示下载失败提示。
    void markAttachmentDownloaded(const QString& key, bool ok);
    void setBodyProgress(const QString& msgId, qint64 received, qint64 expected);
    void clearBodyProgress(const QString& msgId);
    // 正文按需拉取失败时调用：预览区显示错误提示（而非停留在"拉取中 100%"），
    // renderBody 消费该标记后自动清除，下次选中该邮件可重新拉取。
    void setBodyLoadError(const QString& msgId);

    // 切换到附件内联预览面板（MailWidget 在附件下载成功后调用）
    // - msgId/idx    邮件 id + 附件索引
    // - savedPath    本地已落盘的文件
    // - mimeType     附件 MIME（用于决定如何渲染）
    void showAttachmentPreview(const QString& msgId, int idx,
                               const QString& savedPath, const QString& mimeType);

    // 重新发送相关按钮（重新编辑 / 再次发送）可见性。
    // 仅收件箱(INBOX)隐藏，其余文件夹/视图（已发送、草稿、自定义、未读等）显示。
    void setReSendActionsVisible(bool on);
    // "撤销"按钮可见性：仅已发送(Sent)文件夹显示，用于撤回已发邮件。
    void setRevokeActionVisible(bool on);
    // "回复/回复全部/转发"三个按钮可见性：仅在收件箱(INBOX)预览时显示。
    void setReplyForwardActionsVisible(bool on);
    // "删除"按钮可见性：非收件箱文件夹及未读视图中显示（收件箱内隐藏，用列表勾选删除）。
    void setDeleteActionVisible(bool on);

signals:
    void replyRequested();
    void replyAllRequested();                  // "回复全部"按钮
    void forwardRequested();                   // "转发 ▼" 默认项 → 直接转发邮件（原邮件内容作为新邮件正文）
    void forwardAsAttachmentRequested();       // "作为附件转发"下拉项：原邮件 .eml 作为附件 + 正文直接转发
    void forwardOriginalRequested();           // "原件转发"下拉项：原邮件 .eml 作为附件 + 正文简短标识
    void reeditRequested();                    // "重新编辑"按钮：载入写邮件窗口（预填收件人/主题/正文）可修改后重发
    void resendRequested();                    // "再次发送"按钮：不打开编辑器，直接按原收件人/主题/正文原样重发
    void revokeRequested();                    // "撤销"按钮：尝试从已发送文件夹撤回该邮件
    void deleteRequested();                    // 预览区"删除"按钮：同列表删除（删除当前选中邮件/草稿）
    void attachmentClicked(const QString& msgId, int idx, const QString& name);
    void attachmentOpenRequested(const QString& msgId, int idx,
                                 const QString& name, const QString& savedPath);
    void attachmentRevealRequested(const QString& msgId, int idx,
                                   const QString& name, const QString& savedPath);
    // 下载未下载的附件（由前端触发实际 fetchPart）
    void attachmentDownloadRequested(const QString& msgId, int idx,
                                     const QString& name);
    // "另存为"：始终允许（已下载则复制一份；未下载也可选位置边下边存）
    void attachmentSaveAsRequested(const QString& msgId, int idx,
                                   const QString& name, const QString& savedPath);
    void bodyLoadRequested(const QString& msgId);

private slots:
    void onToExpandToggled();
    void onCcExpandToggled();

private:
    QPushButton*        m_replyBtn = nullptr;
    QPushButton*        m_replyAllBtn = nullptr;
    QToolButton*        m_forwardMenuBtn = nullptr;        // "→ 转发 ▼" 下拉按钮（替代原 m_forwardBtn）
    QMenu*              m_forwardMenu = nullptr;           // 转发下拉菜单（3 个 action）
    QPushButton*        m_reeditBtn = nullptr;             // "重新编辑"按钮（写邮件窗口重发）
    QPushButton*        m_resendBtn = nullptr;             // "再次发送"按钮（原样直接重发）
    QPushButton*        m_revokeBtn = nullptr;             // "撤销"按钮（仅已发送文件夹，尝试撤回）
    QPushButton*        m_deleteBtn = nullptr;             // "删除"按钮（非收件箱文件夹/未读视图显示）

    QWidget*            m_headerWidget = nullptr;
    QFormLayout*        m_headerLayout = nullptr;
    QLabel*             m_subjectLabel = nullptr;
    QLabel*             m_fromValueLabel = nullptr;
    QLabel*             m_dateLabel = nullptr;
    QLabel*             m_toValueLabel = nullptr;
    QToolButton*        m_toExpandBtn = nullptr;
    QLabel*             m_ccValueLabel = nullptr;
    QToolButton*        m_ccExpandBtn = nullptr;
    QLabel*             m_ccFieldLabel = nullptr;
    QWidget*            m_ccRowWrap = nullptr;

    QWidget*            m_attachWidget = nullptr;
    QLabel*             m_attachTitleLabel = nullptr;
    QWidget*            m_attachRowsHost = nullptr;
    class FlowLayout*   m_attachRowsLayout = nullptr;

    QStackedWidget*     m_bodyStack = nullptr;
    MailPreviewBrowser* m_previewView = nullptr;
    AttachmentPreviewPane* m_attachPreview = nullptr;  // 附件内联预览面板
    QString             m_attachPreviewKey;            // 当前预览的附件 (msgId|idx)
    QWidget*            m_loadingWidget = nullptr;
    QProgressBar*       m_loadingBar = nullptr;
    QLabel*             m_loadingLabel = nullptr;
    QPlainTextEdit*     m_rawView = nullptr;
    bool                m_showingRaw = false;

    QString m_currentMsgId;
    QSet<QString> m_expandState;
    QSet<QString> m_downloadingAtts;
    QHash<QString, QPair<qint64, qint64>> m_attProgress;
    QHash<QString, QPair<qint64, qint64>> m_bodyProgress;
    QString m_bodyLoadFailMsgId;   // 最近一次正文拉取失败的 msgId（renderBody 消费后清除）
    QHash<QString, QWidget*> m_attRows;
    qint64 m_lastAttPreviewMs = 0;
    qint64 m_lastBodyPreviewMs = 0;

    // 轻量 toast（附件下载成功/失败等瞬时提示）
    QLabel*                 m_toast = nullptr;
    QTimer*                 m_toastTimer = nullptr;
    QGraphicsOpacityEffect* m_toastOpacity = nullptr;
    QPropertyAnimation*     m_toastAnim = nullptr;

    void renderHeader(const MailStore::Message* m);
    void renderAttachments(const MailStore::Message* m);
    void renderBody(const MailStore::Message* m);
    void rebuildAttachmentRows(const MailStore::Message* m);
    void refreshAttachmentRow(const QString& key);
    void setAddressText(QLabel* valLabel, QToolButton* btn,
                        const QStringList& list, const QString& stateKey);
    void throttledRefresh(qint64& lastMs);
    void showRawView(const QString& msgId);
    void onAnchorClicked(const QUrl& url);
    // 默认占位卡片 HTML：图标 + 标题 + 副标题。未选邮件时由 m_previewView 渲染。
    QString buildWelcomeHtml() const;
    QString attachmentKey(int idx) const {
        return m_currentMsgId + QLatin1Char('|') + QString::number(idx);
    }

    // 面板右下角轻量 toast 提示（下载成功/失败的瞬时反馈，自动消失）
    void showToast(const QString& text, bool ok);
    void repositionToast();
    void hideToastNow();

protected:
    void resizeEvent(QResizeEvent* event) override;
};