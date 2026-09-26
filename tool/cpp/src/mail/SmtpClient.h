#pragma once

#include "mail/crypto/MailEncryptor.h"

#include <QString>
#include <QStringList>
#include <QList>

class QSslSocket;

/**
 * SmtpClient: 简单 SMTP 发信客户端（同步、带超时）
 *
 * 支持：
 * - SSL 直连（465）/ STARTTLS（587）/ 明文（25）
 * - AUTH LOGIN 认证
 * - To / Cc / Bcc 多个收件人
 * - 自定义 Message-ID / In-Reply-To / References（回复、转发引用链）
 * - 发件人显示名（From: "Name" <email>）
 * - 富文本正文（multipart/alternative: text/plain + text/html）
 * - 内联图片（multipart/related，HTML 中以 cid:xxx 引用）
 * - 普通附件（multipart/mixed）
 * - 紧急（X-Priority + Importance）
 * - 已读回执请求（Disposition-Notification-To）
 * - 邮件加密（Auto / S/MIME / Password）
 *
 * 加密（见 MailEncryptor）：
 *   encryption.mode = MailEncryptor::Auto   → 收件人都有证书→S/MIME；否则→口令
 *   encryption.mode = MailEncryptor::Smime  → 强制 S/MIME（要求所有收件人有本地公钥）
 *   encryption.mode = MailEncryptor::Password → 强制口令（password 必填）
 *   encryption.mode = MailEncryptor::None   → 不加密（默认）
 */
class SmtpClient {
public:
    struct Attachment {
        QString filePath;   // 本地文件路径
        QString fileName;   // 显示文件名（可空，默认 basename）
        QString contentId;  // 非空 → 内联图片（multipart/related，cid:xxx 引用）
    };

    struct Params {
        QString host;
        int     port = 465;
        bool    ssl  = true;
        QString username;       // 通常为完整邮箱
        QString password;       // 授权码/密码
        QString fromName;       // 发件人显示名（可选）
        QString fromEmail;      // 发件人邮箱（空则用 username）
        QStringList to;
        QStringList cc;
        QStringList bcc;
        QString subject;
        QString body;           // 纯文本正文（始终生成；HTML 模式下作为 multipart/alternative 备用）
        QString htmlBody;       // 可选；非空则启用 multipart/alternative(text, html)
        QList<Attachment> inlineImages;  // 内联图片（HTML 中以 cid:xxx 引用）
        QList<Attachment> attachments;   // 普通附件（multipart/mixed）
        QString inReplyTo;      // 上一封邮件的 Message-ID（回复时）
        QStringList references; // 引用链（转发/回复）
        int     priority = 0;   // 0=正常, 1=紧急(X-Priority: 1 / Importance: High), 2=低(X-Priority: 5)
        bool    readReceipt = false;       // 是否请求已读回执
        QString readReceiptTo;             // 回执接收地址（空则用 fromEmail）

        // 加密策略（见 MailEncryptor::Policy）
        MailEncryptor::Policy encryption;

        // S/MIME 签名：用哪个个人证书（id）给邮件做 detached signature。
        // 空 → 不签名。加密 + 签名可叠加（先签名再加密：multipart/signed → enveloped）。
        QString signingCertId;
        QString signingCertPassword;   // 解锁签名私钥用的 PKCS#12 口令

        // 当 isDispositionNotification=true 时，这封邮件本身就是一个"已读回执"，
        // 内容由 buildDispositionNotificationPayload 构造为 multipart/report；
        // 此时 to 应指向原邮件里 Disposition-Notification-To 的地址（即原邮件发送方）。
        // 其他字段（htmlBody/attachments/inlineImages/priority 等）在 DSN 模式下被忽略。
        bool    isDispositionNotification = false;
        QString dsnOriginalFrom;        // 原邮件 From 头（用于 DSN 报告中"原邮件"信息）
        QString dsnOriginalSubject;     // 原邮件主题
        QString dsnOriginalMessageId;   // 原邮件 Message-ID

        int     timeoutSec = 15;
    };

    // 发送邮件，成功返回 true；失败时 errorMessage 含服务器响应
    static bool send(const Params& params, QString* errorMessage);

private:
    static bool expect(QSslSocket& sock, const QStringList& codes, int timeoutMs, QString* resp);
    static bool sendLine(QSslSocket& sock, const QString& line);
    static QString encodeMimeWord(const QString& s);

    // 构造完整 DATA payload（含所有 MIME part）。供 send() 内部调用，也方便单测。
    // - 仅有 body 且无 htmlBody/inline/attachments → text/plain
    // - 有 htmlBody 但无 inline → multipart/alternative(text, html)
    // - 有 inline → multipart/related(alternative, inline...)
    // - 有 attachments → multipart/mixed(related-or-single, attachments...)
    // 成功返回 true（payload 非空）；失败返回 false（errorMessage 含原因，payload 内容未定义）。
    static bool buildMimePayload(const Params& p, QString& payloadOut, QString* errorMessage);
};