#include "MailEncryptor.h"
#include "CryptoEnvelopes.h"
#include "CertificateManager.h"
#include "SmimeCrypto.h"
#include "PasswordCrypto.h"

#include "core/Logger.h"

#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <QRegularExpression>
#include <QFile>
#include <QUuid>

namespace {
    QString errBuf;

    void setErr(QString* out, const QString& e) {
        if (out) *out = e;
        errBuf = e;
        Logger::instance().error(QString("MailEncryptor: %1").arg(e), "mail");
    }

    // 读 PEM 文件 → X509*（调用方 X509_free）
    X509* readCertFromPemFile(const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return nullptr;
        QByteArray pem = f.readAll();
        f.close();
        BIO* bio = BIO_new_mem_buf(pem.constData(), pem.size());
        X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        return x;
    }

    QString base64Wrap(const QByteArray& bin, int lineLen = 64) {
        QByteArray b64 = bin.toBase64();
        QString out;
        for (int i = 0; i < b64.size(); i += lineLen) {
            out += QString::fromLatin1(b64.mid(i, lineLen));
            out += QStringLiteral("\r\n");
        }
        return out;
    }
}

QString MailEncryptor::modeName(Mode m) {
    switch (m) {
        case None:     return QStringLiteral("none");
        case Auto:     return QStringLiteral("auto");
        case Smime:    return QStringLiteral("smime");
        case Password: return QStringLiteral("password");
    }
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// wrap: 把原始 MIME 包成加密信封
// ─────────────────────────────────────────────────────────────────────────────
bool MailEncryptor::wrap(const QString& originalHeaders,
                         const QString& originalPayload,
                         const Policy& policy,
                         const QStringList& recipientEmails,
                         QString& headersOut,
                         QString& payloadOut,
                         QString* errorOut) {
    if (policy.mode == None) {
        headersOut = originalHeaders;
        payloadOut = originalPayload;
        return true;
    }

    // 拼出待加密的完整 MIME 字符串（headers + 空行 + payload），便于直接喂给 CMS
    QString fullMime = originalHeaders;
    if (!fullMime.endsWith(QStringLiteral("\r\n"))) fullMime += QStringLiteral("\r\n");
    fullMime += QStringLiteral("\r\n");
    fullMime += originalPayload;
    QByteArray mimeBytes = fullMime.toUtf8();

    // 决定实际使用的 mode
    Mode useMode = policy.mode;
    if (useMode == Auto) {
        // Auto: 所有收件人都有本地公钥 → S/MIME；否则 → Password
        bool allHaveCert = !recipientEmails.isEmpty();
        for (const QString& e : recipientEmails) {
            if (CertificateManager::instance().findByEmail(e).id.isEmpty()) {
                allHaveCert = false;
                break;
            }
        }
        useMode = allHaveCert ? Smime : Password;
        Logger::instance().info(QString("MailEncryptor: Auto→%1 (%2 收件人)")
                                .arg(modeName(useMode))
                                .arg(recipientEmails.size()), "mail");
    }

    if (useMode == Password) {
        if (policy.password.isEmpty()) {
            setErr(errorOut, "口令模式但口令为空");
            return false;
        }
        QByteArray envelope = PasswordCrypto::encrypt(policy.password, mimeBytes);
        if (envelope.isEmpty()) {
            setErr(errorOut, "PasswordCrypto::encrypt 失败: " + PasswordCrypto::getLastError());
            return false;
        }

        QString b = QStringLiteral("=_enc_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces).left(16));

        // 顶层 headers
        headersOut = originalHeaders;
        if (!headersOut.endsWith(QStringLiteral("\r\n"))) headersOut += QStringLiteral("\r\n");

        // payload：multipart/alternative(text 说明, application/octet-stream 加密块)
        payloadOut.clear();
        payloadOut += QStringLiteral("Content-Type: multipart/alternative; boundary=\"%1\"\r\n\r\n").arg(b);

        payloadOut += QStringLiteral("--%1\r\n").arg(b);
        payloadOut += QStringLiteral("Content-Type: text/plain; charset=utf-8\r\n");
        payloadOut += QStringLiteral("Content-Transfer-Encoding: 8bit\r\n\r\n");
        payloadOut += QStringLiteral("This message has been encrypted with a password.\r\n");
        payloadOut += QStringLiteral("Fingerprint: %1\r\n").arg(PasswordCrypto::fingerprint(envelope));
        if (!policy.hint.isEmpty())
            payloadOut += QStringLiteral("Hint: %1\r\n").arg(policy.hint);
        payloadOut += QStringLiteral("Use the same password to decrypt in this client.\r\n\r\n");

        payloadOut += QStringLiteral("--%1\r\n").arg(b);
        payloadOut += QStringLiteral(
            "Content-Type: application/octet-stream; name=\"%1\"\r\n")
            .arg(CryptoEnvelopes::ENCRYPTED_FILE_NAME);
        payloadOut += QStringLiteral(
            "Content-Disposition: attachment; filename=\"%1\"\r\n")
            .arg(CryptoEnvelopes::ENCRYPTED_FILE_NAME);
        payloadOut += QStringLiteral(
            "%1: %2\r\n").arg(CryptoEnvelopes::H_ENCRYPTION, CryptoEnvelopes::MODE_PASSWORD_V1);
        payloadOut += QStringLiteral(
            "%1: AES-256-GCM\r\n").arg(CryptoEnvelopes::H_ALGO);
        payloadOut += QStringLiteral(
            "Content-Transfer-Encoding: base64\r\n\r\n");
        payloadOut += base64Wrap(envelope);
        payloadOut += QStringLiteral("\r\n--%1--\r\n").arg(b);
        return true;
    }

    if (useMode == Smime) {
        if (recipientEmails.isEmpty()) {
            setErr(errorOut, "S/MIME 但收件人为空");
            return false;
        }
        // 收集所有收件人的公钥证书，一次性加密：
        //   CMS EnvelopedData 内部为每个收件人生成一份用其公钥加密的会话密钥，
        //   所有收件人都能用各自私钥解开同一密文。
        QList<X509*> rcpts;
        QStringList missing;
        for (const QString& email : recipientEmails) {
            CertificateManager::CertInfo ci = CertificateManager::instance().findByEmail(email);
            if (ci.id.isEmpty()) {
                missing << email;
                continue;
            }
            X509* x = readCertFromPemFile(ci.sourcePath);
            if (!x) {
                setErr(errorOut, QString("读取收件人证书失败: %1").arg(email));
                // 失败时清理已读的
                for (X509* y : rcpts) X509_free(y);
                return false;
            }
            rcpts.append(x);
        }
        if (rcpts.isEmpty()) {
            setErr(errorOut, QString("S/MIME 但这些收件人都无本地证书: %1")
                              .arg(missing.join(", ")));
            return false;
        }
        if (!missing.isEmpty()) {
            Logger::instance().warn(QString("MailEncryptor: S/MIME 跳过缺失证书收件人: %1")
                                    .arg(missing.join(", ")), "mail");
        }
        QByteArray der = SmimeCrypto::encrypt(mimeBytes, rcpts, errorOut);
        for (X509* x : rcpts) X509_free(x);
        if (der.isEmpty()) return false;

        // 顶层 headers
        headersOut = originalHeaders;
        if (!headersOut.endsWith(QStringLiteral("\r\n"))) headersOut += QStringLiteral("\r\n");
        // S/MIME 标准头
        payloadOut.clear();
        payloadOut += QStringLiteral("Content-Type: %1\r\n")
            .arg(CryptoEnvelopes::SMIME_TYPE_ENVELOPED);
        payloadOut += QStringLiteral("Content-Disposition: attachment; filename=\"%1\"\r\n")
            .arg(CryptoEnvelopes::SMIME_FILE_NAME);
        payloadOut += QStringLiteral("Content-Transfer-Encoding: base64\r\n\r\n");
        payloadOut += base64Wrap(der);
        return true;
    }

    setErr(errorOut, "未知 mode");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// detectKind: 嗅探加密类型
// ─────────────────────────────────────────────────────────────────────────────
QString MailEncryptor::detectKind(const QString& topContentType,
                                  const QString& allHeaders,
                                  const QByteArray& payloadBytes) {
    // 1) S/MIME: 顶层 Content-Type: application/pkcs7-mime
    if (topContentType.contains("application/pkcs7-mime", Qt::CaseInsensitive) ||
        topContentType.contains("application/x-pkcs7-mime", Qt::CaseInsensitive)) {
        return QStringLiteral("smime");
    }
    // 2) 口令信封: multipart/alternative 中某 part 头部含 X-Mail-Encryption: password-v1
    if (allHeaders.contains(CryptoEnvelopes::H_ENCRYPTION + ":", Qt::CaseInsensitive)) {
        QRegularExpression re(CryptoEnvelopes::H_ENCRYPTION + ":\\s*(\\S+)",
                              QRegularExpression::CaseInsensitiveOption);
        auto m = re.match(allHeaders);
        if (m.hasMatch() && m.captured(1).trimmed().toLower() == CryptoEnvelopes::MODE_PASSWORD_V1)
            return QStringLiteral("password");
    }
    // 3) 兜底：直接 d2i 试一下（少数 MTA 会剥头）
    if (SmimeCrypto::isSmimeEnvelope(payloadBytes)) return QStringLiteral("smime");
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// unwrap: 根据 detectKind 自动选解密器
// ─────────────────────────────────────────────────────────────────────────────
QByteArray MailEncryptor::unwrap(const QByteArray& mimeBytes,
                                 const QString& password,
                                 QString* errorOut) {
    // 把 mime 切成 headers + body
    int split = mimeBytes.indexOf("\r\n\r\n");
    if (split < 0) split = mimeBytes.indexOf("\n\n");
    if (split < 0) { setErr(errorOut, "找不到 headers/body 分隔"); return {}; }
    int bodyStart = split + (mimeBytes.mid(split, 4) == "\r\n\r\n" ? 4 : 2);
    QByteArray headers = mimeBytes.left(split);
    QByteArray body    = mimeBytes.mid(bodyStart);

    QString topCtype;
    static const QRegularExpression ctypeRe("^Content-Type:\\s*(.+)$",
                                            QRegularExpression::CaseInsensitiveOption |
                                            QRegularExpression::MultilineOption);
    auto cm = ctypeRe.match(QString::fromLatin1(headers));
    if (cm.hasMatch()) topCtype = cm.captured(1).trimmed();

    QString kind = detectKind(topCtype, QString::fromLatin1(headers), body);

    if (kind == "smime") {
        // base64 解码 body
        QByteArray der = QByteArray::fromBase64(body);
        // 找到我的个人证书 + 私钥
        auto personals = CertificateManager::instance().listPersonal();
        if (personals.isEmpty()) {
            setErr(errorOut, "无个人证书，无法解密 S/MIME");
            return {};
        }
        CertificateManager::CertInfo pi = personals.first();
        EVP_PKEY* pkey = CertificateManager::instance().unlockPersonalKey(pi.id, password);
        if (!pkey) {
            setErr(errorOut, "解锁私钥失败: " + CertificateManager::getLastError());
            return {};
        }
        X509* cert = CertificateManager::instance().getPersonalCert(pi.id);
        QByteArray mime = SmimeCrypto::decrypt(der, pkey, cert, errorOut);
        EVP_PKEY_free(pkey);
        if (cert) X509_free(cert);
        return mime;
    }

    if (kind == "password") {
        // 从 multipart/alternative 中找出 X-Mail-Encryption part 的 base64 body
        QByteArray b64body;
        // 简化：抓最后一段 base64（multipart 末 part 是加密块）
        QRegularExpression splitParts(
            QStringLiteral("--%1").arg(QStringLiteral("(?<bnd>[^\\s]+)")));
        // 实际更可靠：按 boundary 切分
        // 解析 multipart/alternative
        QRegularExpression ctypeAll(QStringLiteral(
            "Content-Type:\\s*multipart/alternative;\\s*boundary=\"?(?<bnd>[^\\s;]+)\"?"),
            QRegularExpression::CaseInsensitiveOption);
        auto m = ctypeAll.match(QString::fromLatin1(headers));
        if (!m.hasMatch()) {
            setErr(errorOut, "不是 multipart/alternative");
            return {};
        }
        QString bnd = m.captured("bnd");
        QRegularExpression partRe(QStringLiteral("\\r?\\n--%1(?:\\r?\\n|--)").arg(QRegularExpression::escape(bnd)));
        QStringList parts = QString::fromLatin1(body).split(partRe, Qt::KeepEmptyParts);
        // parts[0] 通常为空，parts[1]=text 说明，parts[2]=加密 part
        for (int i = parts.size() - 1; i >= 1; --i) {
            QString p = parts[i];
            if (p.contains(CryptoEnvelopes::H_ENCRYPTION, Qt::CaseInsensitive)) {
                // 取出 base64
                int hdrEnd = p.indexOf(QStringLiteral("\r\n\r\n"));
                if (hdrEnd < 0) hdrEnd = p.indexOf(QStringLiteral("\n\n"));
                if (hdrEnd < 0) continue;
                int bodyFrom = hdrEnd + (p.mid(hdrEnd, 4) == QStringLiteral("\r\n\r\n") ? 4 : 2);
                QString b64 = p.mid(bodyFrom);
                // 去换行
                b64.remove(QChar('\r')); b64.remove(QChar('\n'));
                b64body = b64.toLatin1();
                break;
            }
        }
        if (b64body.isEmpty()) {
            setErr(errorOut, "未找到加密 part");
            return {};
        }
        QByteArray envelope = QByteArray::fromBase64(b64body);
        if (!PasswordCrypto::isPasswordEnvelope(envelope)) {
            setErr(errorOut, "信封魔数错误");
            return {};
        }
        QByteArray mime = PasswordCrypto::decrypt(password, envelope);
        if (mime.isEmpty()) {
            setErr(errorOut, "解密失败: " + PasswordCrypto::getLastError());
        }
        return mime;
    }

    setErr(errorOut, "未识别为加密邮件");
    return {};
}