#include "mail/SmtpClient.h"
#include "core/Logger.h"
#include "mail/crypto/CertificateManager.h"
#include "mail/crypto/SmimeCrypto.h"

#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#include <QSslSocket>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QDateTime>
#include <QRegularExpression>

namespace {

// base64 编码并按 76 列换行（MIME 规范）
QString wrapBase64(const QByteArray& bytes, int wrapAt = 76) {
    QString out = QString::fromLatin1(bytes.toBase64());
    QString wrapped;
    for (int i = 0; i < out.size(); i += wrapAt)
        wrapped += out.mid(i, wrapAt) + "\r\n";
    return wrapped;
}

// 转义 HTML → multipart/alternative 中的 text/plain（保留原样，纯文本正文本身已是 plain）
QString escapeForPlain(const QString& s) {
    return s; // 用户传入的 plain 不做转义
}

// MIME 头字段值里出现的中文/非 ASCII 用 RFC 2047 (B 编码) 包裹
QString encodeHeader(const QString& s) {
    static const QRegularExpression re(QStringLiteral("[^\\x20-\\x7e]"));
    if (!re.match(s).hasMatch()) return s;
    return QStringLiteral("=?UTF-8?B?%1?=").arg(QString::fromUtf8(s.toUtf8().toBase64()));
}

// 从文件读字节；若 filePath 不存在或读失败，errorMessage 非空并返回空字节数组
QByteArray readAttachmentBytes(const QString& filePath, QString* errorMessage) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = QStringLiteral("无法读取附件: %1").arg(filePath);
        return {};
    }
    return f.readAll();
}

} // namespace

bool SmtpClient::sendLine(QSslSocket& sock, const QString& line) {
    sock.write(line.toUtf8() + "\r\n");
    return sock.waitForBytesWritten(5000);
}

bool SmtpClient::expect(QSslSocket& sock, const QStringList& codes, int timeoutMs, QString* resp) {
    QString response;
    while (true) {
        if (!sock.waitForReadyRead(timeoutMs)) {
            *resp = response.isEmpty() ? QString("等待服务器响应超时") : response;
            return false;
        }
        response += QString::fromUtf8(sock.readAll());
        int lastNl = response.lastIndexOf("\r\n", response.length() - 3);
        QString lastLine = (lastNl >= 0) ? response.mid(lastNl + 2) : response;
        if (lastLine.length() >= 4 && lastLine[3] == ' ') break;
        if (response.endsWith("\r\n") && lastLine.length() < 4) break;
    }
    *resp = response.trimmed();
    return codes.contains(response.left(3));
}

QString SmtpClient::encodeMimeWord(const QString& s) {
    return "=?UTF-8?B?" + QString::fromUtf8(s.toUtf8().toBase64()) + "?=";
}

bool SmtpClient::send(const Params& p, QString* errorMessage) {
    int timeoutMs = p.timeoutSec * 1000;
    QSslSocket sock;
    QObject::connect(&sock, &QSslSocket::sslErrors, &sock, [&sock](const QList<QSslError>&) {
        sock.ignoreSslErrors();
    });

    if (p.ssl) {
        sock.connectToHostEncrypted(p.host, static_cast<quint16>(p.port));
        if (!sock.waitForEncrypted(timeoutMs)) {
            *errorMessage = QString("SSL 连接失败: %1").arg(sock.errorString());
            return false;
        }
    } else {
        sock.connectToHost(p.host, static_cast<quint16>(p.port));
        if (!sock.waitForConnected(timeoutMs)) {
            *errorMessage = QString("连接失败: %1").arg(sock.errorString());
            return false;
        }
    }

    QString resp;
    if (!expect(sock, {"220"}, timeoutMs, &resp)) {
        *errorMessage = "SMTP 握手失败: " + resp;
        return false;
    }

    QString ehlo = "EHLO desktool.local";
    sendLine(sock, ehlo);
    if (!expect(sock, {"250"}, timeoutMs, &resp)) {
        *errorMessage = "EHLO 被拒绝: " + resp;
        return false;
    }

    if (!p.ssl) {
        if (resp.contains("STARTTLS", Qt::CaseInsensitive)) {
            sendLine(sock, "STARTTLS");
            if (!expect(sock, {"220"}, timeoutMs, &resp)) {
                *errorMessage = "STARTTLS 失败: " + resp;
                return false;
            }
            sock.startClientEncryption();
            if (!sock.waitForEncrypted(timeoutMs)) {
                *errorMessage = "TLS 加密协商失败: " + sock.errorString();
                return false;
            }
            sendLine(sock, ehlo);
            if (!expect(sock, {"250"}, timeoutMs, &resp)) {
                *errorMessage = "TLS 后 EHLO 被拒绝: " + resp;
                return false;
            }
        }
    }

    if (!p.username.isEmpty()) {
        sendLine(sock, "AUTH LOGIN");
        if (!expect(sock, {"334"}, timeoutMs, &resp)) {
            *errorMessage = "服务器不支持 AUTH LOGIN: " + resp;
            return false;
        }
        sendLine(sock, QString::fromUtf8(p.username.toUtf8().toBase64()));
        if (!expect(sock, {"334"}, timeoutMs, &resp)) {
            *errorMessage = "用户名被拒绝: " + resp;
            return false;
        }
        sendLine(sock, QString::fromUtf8(p.password.toUtf8().toBase64()));
        if (!expect(sock, {"235"}, timeoutMs, &resp)) {
            *errorMessage = "认证失败（检查账号/授权码）: " + resp;
            return false;
        }
    }

    QString fromEmail = p.fromEmail.isEmpty() ? p.username : p.fromEmail;
    sendLine(sock, QString("MAIL FROM:<%1>").arg(fromEmail));
    if (!expect(sock, {"250"}, timeoutMs, &resp)) {
        *errorMessage = "MAIL FROM 被拒绝: " + resp;
        return false;
    }

    auto declareRcpt = [&](const QString& addr) -> bool {
        sendLine(sock, QString("RCPT TO:<%1>").arg(addr.trimmed()));
        if (!expect(sock, {"250", "251"}, timeoutMs, &resp)) {
            *errorMessage = QString("收件人 %1 被拒绝: %2").arg(addr, resp);
            return false;
        }
        return true;
    };
    for (const QString& t : p.to)  if (!declareRcpt(t)) return false;
    for (const QString& t : p.cc)  if (!declareRcpt(t)) return false;
    for (const QString& t : p.bcc) if (!declareRcpt(t)) return false;

    sendLine(sock, "DATA");
    if (!expect(sock, {"354"}, timeoutMs, &resp)) {
        *errorMessage = "DATA 被拒绝: " + resp;
        return false;
    }

    // ── 构造 MIME 邮件 ──
    QStringList lines;
    // Message-ID（没有给出则生成）
    QString messageId = p.inReplyTo;
    QString midHeader = messageId;
    if (midHeader.isEmpty()) {
        midHeader = QString("<%1@desktool>").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    lines << QString("Message-ID: %1").arg(midHeader);
    lines << QString("Date: %1").arg(QDateTime::currentDateTimeUtc().toString("ddd, d MMM yyyy HH:mm:ss") + " +0000");

    // From: "DisplayName" <email>
    QString fromHeader;
    if (!p.fromName.isEmpty()) {
        fromHeader = QString("%1 <%2>").arg(encodeMimeWord(p.fromName), fromEmail);
    } else {
        fromHeader = QString("<%1>").arg(fromEmail);
    }
    lines << "From: " + fromHeader;

    auto joinAddrs = [](const QStringList& addrs) -> QString {
        QStringList parts;
        for (const QString& a : addrs) parts << QString("<%1>").arg(a.trimmed());
        return parts.join(", ");
    };
    if (!p.to.isEmpty())  lines << "To: " + joinAddrs(p.to);
    if (!p.cc.isEmpty())  lines << "Cc: " + joinAddrs(p.cc);
    // DSN 模式：subject 自动加 "Read receipt:" 前缀（让收件方一眼能看出）
    QString subj = p.subject;
    if (p.isDispositionNotification) {
        if (!subj.startsWith(QStringLiteral("Read receipt:"), Qt::CaseInsensitive))
            subj = QStringLiteral("Read receipt: %1").arg(subj);
    }
    lines << "Subject: " + encodeMimeWord(subj);
    lines << "MIME-Version: 1.0";

    if (!p.inReplyTo.isEmpty()) {
        lines << QString("In-Reply-To: %1").arg(p.inReplyTo);
    }
    if (!p.isDispositionNotification && !p.references.isEmpty()) {
        lines << "References: " + p.references.join(" ");
    }

    // 优先级 / 紧急 / 已读回执请求
    if (p.priority == 1) {
        lines << QStringLiteral("X-Priority: 1 (Highest)");
        lines << QStringLiteral("Importance: High");
    } else if (p.priority == 2) {
        lines << QStringLiteral("X-Priority: 5 (Lowest)");
        lines << QStringLiteral("Importance: Low");
    }
    if (p.readReceipt) {
        QString rto = p.readReceiptTo.isEmpty() ? fromEmail : p.readReceiptTo;
        lines << QStringLiteral("Disposition-Notification-To: %1").arg(rto);
    }

    // 委托 MIME payload 构造（支持 html / inline / mixed 多种结构）
    QString payload;
    if (!buildMimePayload(p, payload, errorMessage)) {
        return false;
    }

    // ── S/MIME 签名（可选，先于加密）──
    // 流程：原 MIME → multipart/signed(原 MIME, pkcs7-signature)。
    // 加密会把这整封带签名的邮件再包一层 enveloped。
    if (!p.signingCertId.isEmpty()) {
        using CertMgr = CertificateManager;
        CertMgr::CertInfo ci = CertMgr::instance().listPersonal().isEmpty()
            ? CertMgr::CertInfo{}
            : CertMgr::CertInfo{};
        // 按 id 查找
        for (const auto& c : CertMgr::instance().listPersonal()) {
            if (c.id == p.signingCertId) { ci = c; break; }
        }
        if (ci.id.isEmpty()) {
            if (errorMessage) *errorMessage =
                QStringLiteral("签名失败：找不到个人证书 id=%1").arg(p.signingCertId);
            return false;
        }
        if (p.signingCertPassword.isEmpty()) {
            if (errorMessage) *errorMessage =
                QStringLiteral("签名失败：未提供 PKCS#12 口令");
            return false;
        }
        EVP_PKEY* pkey = CertMgr::instance().unlockPersonalKey(p.signingCertId, p.signingCertPassword);
        if (!pkey) {
            if (errorMessage) *errorMessage =
                QStringLiteral("签名失败：解锁私钥失败: %1").arg(CertMgr::getLastError());
            return false;
        }
        X509* x509 = CertMgr::instance().getPersonalCert(p.signingCertId);
        if (!x509) {
            EVP_PKEY_free(pkey);
            if (errorMessage) *errorMessage =
                QStringLiteral("签名失败：读取个人证书失败");
            return false;
        }
        // 完整 MIME = lines + 空行 + payload
        QByteArray fullMime = (lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n\r\n") + payload).toUtf8();
        QByteArray sigDer = SmimeCrypto::sign(fullMime, x509, pkey, errorMessage);
        X509_free(x509);
        EVP_PKEY_free(pkey);
        if (sigDer.isEmpty()) return false;

        QString b = QStringLiteral("=_sig_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces).left(16));

        // 新 payload：multipart/signed(原 MIME part, 签名 part)
        QString newPayload;
        newPayload += QStringLiteral("--%1\r\n").arg(b);
        // 原 MIME part 用 message/rfc822 容器把完整原邮件包进去（最稳妥的标准做法）
        newPayload += QStringLiteral("Content-Type: message/rfc822; charset=utf-8\r\n");
        newPayload += QStringLiteral("Content-Disposition: inline\r\n");
        newPayload += QStringLiteral("\r\n");
        newPayload += QString::fromUtf8(fullMime);
        newPayload += QStringLiteral("\r\n");

        newPayload += QStringLiteral("--%1\r\n").arg(b);
        newPayload += QStringLiteral("Content-Type: application/pkcs7-signature; name=\"smime.p7s\"\r\n");
        newPayload += QStringLiteral("Content-Transfer-Encoding: base64\r\n");
        newPayload += QStringLiteral("Content-Disposition: attachment; filename=\"smime.p7s\"\r\n\r\n");
        newPayload += wrapBase64(sigDer, 76);
        newPayload += QStringLiteral("\r\n--%1--\r\n").arg(b);

        // 更新 payload + 在 lines 中覆盖 Content-Type
        payload = QStringLiteral(
            "Content-Type: multipart/signed; protocol=\"application/pkcs7-signature\"; "
            "micalg=\"sha-256\"; boundary=\"%1\"\r\n\r\n%2").arg(b, newPayload);
        // 把 lines 里旧的 Content-Type 头去掉（如果有），避免重复
        QStringList newLines;
        for (const QString& ln : lines) {
            if (!ln.startsWith(QStringLiteral("Content-Type:"), Qt::CaseInsensitive)) {
                newLines << ln;
            }
        }
        lines = newLines;

        Logger::instance().info("SmtpClient: 已对邮件做 S/MIME 签名", "mail");
    }

    // ── 加密（MailEncryptor）──
    // 邮件加密是"整封邮件"加密（headers + payload 一起），输出新的顶层 headers + payload。
    // None 模式：原样透传。
    QStringList allRcpts = p.to + p.cc; // bcc 不出现在顶层 Rcpt 列表里，但加密需要包含
    if (!p.bcc.isEmpty()) allRcpts += p.bcc;

    QString encryptedHeaders, encryptedPayload;
    if (!MailEncryptor::wrap(lines.join(QStringLiteral("\r\n")),
                             payload,
                             p.encryption,
                             allRcpts,
                             encryptedHeaders, encryptedPayload,
                             errorMessage)) {
        return false;
    }

    // 头部 + 空行 + payload
    // DATA payload 需要点号转义（每行首字符是 "." 时变为 ".."）
    QString data = encryptedHeaders + QStringLiteral("\r\n\r\n") + encryptedPayload;
    data.replace(QStringLiteral("\r\n."), QStringLiteral("\r\n.."));
    sendLine(sock, data + QStringLiteral("\r\n."));

    if (!expect(sock, {"250"}, timeoutMs, &resp)) {
        *errorMessage = "邮件正文被拒绝: " + resp;
        return false;
    }

    sendLine(sock, "QUIT");
    sock.waitForBytesWritten(3000);
    sock.disconnectFromHost();

    Logger::instance().success(QString("邮件已发送: %1 -> [%2] 附件=%3")
        .arg(fromEmail, allRcpts.join(","), QString::number(p.attachments.size())), "mail");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildMimePayload — 构造完整的 MIME 正文（DATA payload，不含头）
//
// 结构组合（自外向内）：
//   - 仅正文(纯文本)         →  Content-Type: text/plain
//   - 正文 + htmlBody         →  multipart/alternative(text/plain, text/html)
//   - 正文 + htmlBody + inline →  multipart/related(alternative, image/*...)
//   - 正文 + 普通附件          →  multipart/mixed(单 part 或 alternative/related, attachment...)
//   - 正文 + 内联 + 附件       →  multipart/mixed(multipart/related, attachment...)
//
// 所有 boundary 在构造时生成；本函数只构造 *正文 payload*，头字段由调用方（send）拼到前面。
// 返回 false 表示附件读失败（errorMessage 含原因）。
// ─────────────────────────────────────────────────────────────────────────────
bool SmtpClient::buildMimePayload(const Params& p, QString& payloadOut, QString* errorMessage) {
    payloadOut.clear();
    const bool hasAlt     = !p.htmlBody.isEmpty();
    const bool hasInline  = !p.inlineImages.isEmpty();
    const bool hasAttach  = !p.attachments.isEmpty();

    // 工具：生成 multipart boundary（提到顶部供 DSN 分支先用）
    auto newBoundary = []() {
        return QStringLiteral("=_part_%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(16));
    };

    // ── DSN (Disposition-Notification) 特殊分支 ──
    // 构造 multipart/report; report-type=disposition-notification；两份 part：
    //   1) text/plain           人类可读的简短说明
    //   2) message/disposition-notification   RFC 8098 机器可读字段
    if (p.isDispositionNotification) {
        QString b = newBoundary();
        QString payload;
        payload += QStringLiteral("Content-Type: multipart/report; "
                                  "report-type=disposition-notification; boundary=\"%1\"\r\n\r\n")
            .arg(b);

        // Part 1: 人类可读文本（包含原邮件主题 + From）
        payload += QStringLiteral("--%1\r\n").arg(b);
        payload += QStringLiteral("Content-Type: text/plain; charset=utf-8\r\n");
        payload += QStringLiteral("Content-Transfer-Encoding: 8bit\r\n\r\n");
        payload += QStringLiteral("Your message\r\n\r\n");
        if (!p.dsnOriginalFrom.isEmpty())
            payload += QStringLiteral("    From: %1\r\n").arg(p.dsnOriginalFrom);
        if (!p.dsnOriginalSubject.isEmpty())
            payload += QStringLiteral("    Subject: %1\r\n").arg(p.dsnOriginalSubject);
        if (!p.dsnOriginalMessageId.isEmpty())
            payload += QStringLiteral("    Message-ID: %1\r\n").arg(p.dsnOriginalMessageId);
        payload += QStringLiteral("\r\nhas been displayed (read).\r\n");

        // Part 2: message/disposition-notification（机器可读）
        payload += QStringLiteral("\r\n--%1\r\n").arg(b);
        payload += QStringLiteral("Content-Type: message/disposition-notification\r\n\r\n");
        payload += QStringLiteral("Reporting-UA: bambooRat; Qt mail client\r\n");
        if (!p.fromEmail.isEmpty())
            payload += QStringLiteral("Final-Recipient: rfc822; %1\r\n").arg(p.fromEmail);
        if (!p.dsnOriginalMessageId.isEmpty())
            payload += QStringLiteral("Original-Message-ID: %1\r\n").arg(p.dsnOriginalMessageId);
        payload += QStringLiteral("Disposition: manual-action/MDN-sent-automatically; displayed\r\n");
        payload += QStringLiteral("\r\n--%1--\r\n").arg(b);

        payloadOut = payload;
        return true;
    }

    // 工具：生成 multipart boundary（提到顶部供 DSN 分支先用）
    //（已上移）

    // 辅助：构造 multipart/alternative(text, html) 的内容（不含外层边界）
    auto buildAlternative = [&](const QString& b) -> QString {
        QString body;
        body += QStringLiteral("--%1\r\n").arg(b);
        body += QStringLiteral("Content-Type: text/plain; charset=utf-8\r\n");
        body += QStringLiteral("Content-Transfer-Encoding: 8bit\r\n\r\n");
        body += escapeForPlain(p.body);
        if (!body.endsWith(QStringLiteral("\r\n"))) body += QStringLiteral("\r\n");
        body += QStringLiteral("\r\n--%1\r\n").arg(b);
        body += QStringLiteral("Content-Type: text/html; charset=utf-8\r\n");
        body += QStringLiteral("Content-Transfer-Encoding: 8bit\r\n\r\n");
        body += p.htmlBody;
        if (!body.endsWith(QStringLiteral("\r\n"))) body += QStringLiteral("\r\n");
        return body;
    };

    // 辅助：构造 multipart/related(alternative-or-text, inline images...) 的内容
    // 返回完整的 related 块（含最后 --<b>--），并自动在外层拼上 Content-Type 头（不含主头空行）
    auto buildRelated = [&](const QString& b) -> QString {
        QString body;
        if (hasAlt) {
            // 内嵌 alternative（同一 boundary）
            body += buildAlternative(b);
        } else {
            // 仅纯文本
            body += QStringLiteral("--%1\r\n").arg(b);
            body += QStringLiteral("Content-Type: text/plain; charset=utf-8\r\n");
            body += QStringLiteral("Content-Transfer-Encoding: 8bit\r\n\r\n");
            body += escapeForPlain(p.body);
            if (!body.endsWith(QStringLiteral("\r\n"))) { body.append(QStringLiteral("\r\n")); }
        }
        for (const auto& img : p.inlineImages) {
            QByteArray bytes = readAttachmentBytes(img.filePath, errorMessage);
            if (bytes.isEmpty()) {
                if (errorMessage && !errorMessage->isEmpty()) {
                    payloadOut.clear();
                    return QString();   // buildRelated lambda 返回 QString；失败用空串
                }
            }
            QString cid  = img.contentId;
            QString name = img.fileName.isEmpty() ? QFileInfo(img.filePath).fileName() : img.fileName;
            body += QStringLiteral("\r\n--%1\r\n").arg(b);
            body += QStringLiteral("Content-Type: application/octet-stream; name=\"%1\"\r\n").arg(name);
            body += QStringLiteral("Content-Disposition: inline; filename=\"%1\"\r\n").arg(name);
            if (!cid.isEmpty())
                body += QStringLiteral("Content-ID: <%1>\r\n").arg(cid);
            body += QStringLiteral("Content-Transfer-Encoding: base64\r\n\r\n");
            body += wrapBase64(bytes);
        }
        body += QStringLiteral("--%1--\r\n").arg(b);
        return body;
    };

    QString payload;

    if (!hasAttach && !hasInline) {
        // ── 简单情况：没有内联、没有附件 ──
        if (hasAlt) {
            // multipart/alternative(text, html)
            QString b = newBoundary();
            payload += QStringLiteral("Content-Type: multipart/alternative; boundary=\"%1\"\r\n\r\n")
                .arg(b);
            payload += buildAlternative(b);
            payload += QStringLiteral("--%1--\r\n").arg(b);
        } else {
            // 单 part text/plain
            payload += QStringLiteral("Content-Type: text/plain; charset=utf-8\r\n");
            payload += QStringLiteral("Content-Transfer-Encoding: 8bit\r\n\r\n");
            payload += p.body;
        }
        payloadOut = payload;
        return true;
    }

    // ── 复杂情况：需要 outer multipart ──
    if (hasAttach) {
        // 外层 multipart/mixed：包含 related（可选）+ 多个 attachment
        QString b = newBoundary();
        payload += QStringLiteral("Content-Type: multipart/mixed; boundary=\"%1\"\r\n\r\n")
            .arg(b);

        if (hasInline) {
            // 嵌入 related 子块
            payload += buildRelated(b);
        } else {
            // 仅 alternative 或 text
            if (hasAlt) {
                payload += buildAlternative(b);
            } else {
                payload += QStringLiteral("--%1\r\n").arg(b);
                payload += QStringLiteral("Content-Type: text/plain; charset=utf-8\r\n");
                payload += QStringLiteral("Content-Transfer-Encoding: 8bit\r\n\r\n");
                payload += escapeForPlain(p.body);
                if (!payload.endsWith(QStringLiteral("\r\n"))) payload += QStringLiteral("\r\n");
            }
        }

        // 普通附件
        for (const auto& att : p.attachments) {
            QByteArray bytes = readAttachmentBytes(att.filePath, errorMessage);
            if (bytes.isEmpty()) {
                if (errorMessage && !errorMessage->isEmpty()) {
                    payloadOut.clear();
                    return false;
                }
            }
            QString fname = att.fileName.isEmpty() ? QFileInfo(att.filePath).fileName() : att.fileName;
            payload += QStringLiteral("\r\n--%1\r\n").arg(b);
            payload += QStringLiteral("Content-Type: application/octet-stream; name=\"%1\"\r\n").arg(fname);
            payload += QStringLiteral("Content-Disposition: attachment; filename=\"%1\"\r\n").arg(fname);
            payload += QStringLiteral("Content-Transfer-Encoding: base64\r\n\r\n");
            payload += wrapBase64(bytes);
        }
        payload += QStringLiteral("--%1--\r\n").arg(b);
    } else {
        // 只有 inline，没有附件 → 外层 multipart/related
        QString b = newBoundary();
        payload += QStringLiteral("Content-Type: multipart/related; boundary=\"%1\"\r\n\r\n")
            .arg(b);
        payload += buildRelated(b);
    }

    payloadOut = payload;
    return true;
}
