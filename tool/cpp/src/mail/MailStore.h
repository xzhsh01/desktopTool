#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QList>
#include <QUuid>

/**
 * MailStore: 邮件与草稿的本地存储
 *
 * - 草稿：drafts.json（用户主动保存的草稿邮件）
 * - 邮件缓存：messages.json（拉取的收件箱 / 已发送，用于离线查看与提醒去重）
 *
 * 文件位于 %APPDATA%/KFrame/bambooRat/mail/
 */
class MailStore : public QObject {
    Q_OBJECT

public:
    struct Draft {
        QString id;
        QString accountId;
        QStringList to;
        QStringList cc;
        QStringList bcc;
        QString subject;
        QString body;          // 纯文本（Markdown 模式下为 Markdown 源码；富文本模式下为 toPlainText fallback）
        QString htmlBody;      // 可选；非空时编辑器会还原富文本
        bool     markdownMode = false;  // 编辑器是否处于 Markdown 模式（恢复草稿时识别）
        QStringList attachmentPaths;         // 编辑器附件磁盘路径
        QStringList attachmentDisplayNames;  // 列表显示名（与 paths 一一对应；为空则用 basename）
        QString inReplyToId;   // 关联的邮件 id（回复/转发时）
        QString forwardOfId;   // 转发时的原邮件 id
        QDateTime updatedAt;
    };

    struct Attachment {
        QString name;          // 原始文件名（RFC2047 已解码）
        QString mimeType;      // 如 "application/pdf" / "image/png"
        QString encoding;      // "base64" / "quoted-printable" / "7bit" / "8bit" / "binary"
        qint64  size = 0;      // 解码后字节数（base64 按 3/4 估算）
        QString section;       // IMAP BODY[section] 编号（如 "2" / "1.2"）
    };

    struct Message {
        QString id;
        QString accountId;
        QString folder;        // "INBOX" / "Sent" / "Drafts" / "Trash"
        QString messageId;     // IMAP Message-ID
        QString imapUid;       // IMAP UID（用于 fetchBody / fetchRawSource / markSeen / markDeleted）
        QString from;
        QStringList to;
        QStringList cc;
        QString subject;
        QString body;          // 解析后的纯文本正文
        QString htmlBody;      // text/html 正文原样保留（富文本预览渲染用）
        QByteArray rawSource;  // 整封原始 MIME 原文（"查看原件"时显示）
        QDateTime date;
        bool read = false;
        bool starred = false;
        QList<Attachment> attachments;   // 附件元数据（文件名/类型/大小/section，供点击下载）
    };

    static MailStore& instance();

    // ── 草稿 ────────────────────────────────────────
    const QList<Draft>& drafts() const { return m_drafts; }
    Draft* draft(const QString& id);
    Draft upsertDraft(const Draft& d);                // 不存在则新增；返回完整草稿（含 id）
    bool removeDraft(const QString& id);

    // ── 邮件缓存 ────────────────────────────────────
    const QList<Message>& messages() const { return m_messages; }
    QList<Message> messagesIn(const QString& accountId, const QString& folder) const;
    Message* message(const QString& id);
    int unreadCount(const QString& accountId, const QString& folder = "INBOX") const;
    int countIn(const QString& accountId, const QString& folder) const;   // 文件夹内邮件总数（本地缓存）
    // upsert：按 messageId（IMAP Message-ID）去重，若无则用 id
    void upsertMessages(const QList<Message>& list);
    void markRead(const QString& id, bool read);
    bool removeMessage(const QString& id);
    // 删除指定账号的全部缓存邮件（账号被删除时调用，避免孤儿数据残留）
    int removeMessagesByAccount(const QString& accountId);
    // 清理孤儿邮件：accountId 不在有效账号列表中的缓存邮件（如自测遗留）
    int removeOrphanMessages(const QStringList& validAccountIds);
    // 仅更新正文（正文拉取后回写，纯文本+HTML 一起更新）；返回是否成功
    bool updateBody(const QString& id, const QString& body, const QString& htmlBody,
                    const QList<Attachment>& attachments = {});
    // 仅更新 rawSource（原始 MIME 拉取后回写）；返回是否成功
    bool updateRawSource(const QString& id, const QByteArray& raw);
    // 写入 raw + 回填 messageId（ENVELOPE 漏给时从 raw header 解析）；返回是否成功
    bool updateRawSource(const QString& id, const QByteArray& raw,
                         const QString& fillMessageId);
    // 清空正文+附件+rawSource（强制重拉前的"清旧值"，触发 bodyUpdated/rawSourceUpdated 让预览区回到占位）
    bool clearBody(const QString& id);

    // ── 已读回执 (DSN) 询问状态（运行期缓存，不持久化） ──
    // rawSource 写入后已检测并询问过用户的 messageId 集合（避免重复弹窗）
    // 用 QSet 保证 O(1) 查询。跨进程不保留——重启后用户会再被问一次，无副作用。
    bool hasHandledReadReceipt(const QString& id) const;
    void markHandledReadReceipt(const QString& id);
    // 解析 rawSource 中的 Disposition-Notification-To 头；空表示无回执请求
    // 用 rawSource 的原因：smtp/imap 服务器把头字段放在 raw 字节里，MailStore
    // 不解析 / 不缓存头字段，所以调用方需自己提供 raw。
    static QString extractReadReceiptAddress(const QByteArray& rawSource);

signals:
    void draftsChanged();
    void messagesChanged();
    void newMailArrived(const QString& accountId, int unreadCount);
    // rawSource 更新专用细粒度信号（避免触发整个邮件列表刷新）
    void rawSourceUpdated(const QString& messageId);
    // 正文到达专用细粒度信号：触发预览区刷新但不重建列表
    void bodyUpdated(const QString& messageId);
    // rawSource 到达后检测到 Disposition-Notification-To 头，请求发送已读回执
    //   id = MailStore::Message::id
    //   rto = 头值（"DisplayName <email@x>" 或 "email@x"）
    void readReceiptRequested(const QString& id, const QString& rto);

private:
    MailStore(QObject* parent = nullptr);
    MailStore(const MailStore&) = delete;
    MailStore& operator=(const MailStore&) = delete;

    void loadDrafts();
    void saveDrafts();
    void loadMessages();
    void saveMessages();

    QList<Draft>    m_drafts;
    QList<Message>  m_messages;
    QString         m_draftsPath;
    QString         m_messagesPath;
    QSet<QString>   m_handledReadReceiptIds;   // 运行期缓存，不写盘
};
