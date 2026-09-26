#pragma once

#include <QString>
#include <QByteArray>
#include <QStringList>

/**
 * MailEncryptor: 邮件加密顶层接口
 *
 * 策略：
 *   - 优先用 S/MIME（X.509 公钥）：若收件人在本地证书库有匹配的 email 公钥
 *   - 否则回退到口令加密（发送方设口令，接收方用同一口令解开）
 *   - 也可强制指定 mode
 *
 * 输出：
 *   - encrypt() 返回 MIME 顶层 headers + payload，可直接拼到 SMTP DATA
 *   - decrypt() 反过来：检测加密类型后调用对应解密器
 *
 * 用法（发送端 SmtpClient）：
 *   SmtpClient::Params p = ...;
 *   p.encryption.mode = MailEncryptor::Auto;        // 自动
 *   p.encryption.password = "shared-secret";       // 口令方案口令
 *   QString headers, payload;
 *   if (!MailEncryptor::wrap(p, headers, payload, &err)) { ... }
 *   // headers 即 Content-Type 等顶层头（不含 From/To/Subject）
 *   // payload 即整个 MIME 正文（已包含 Content-Type 头）
 *
 * 用法（接收端）：
 *   QByteArray mime = MailEncryptor::unwrap(rawMime, &passwordInput, &err);
 */
class MailEncryptor {
public:
    enum Mode {
        None       = 0,   // 不加密
        Auto       = 1,   // 自动：S/MIME 优先 → 口令回退
        Smime      = 2,   // 强制 S/MIME（要求所有收件人都有本地公钥）
        Password   = 3,   // 强制口令
    };

    struct Policy {
        Mode    mode          = None;
        QString password;          // 口令方案口令
        QString hint;              // 给接收方的提示（可选，写入 X-Mail-Encryption-Hint）
    };

    // ── 发送端 ──
    /**
     * 把原始 MIME (headers + payload) 用指定策略加密，得到可直接发送的顶层 headers + payload
     * @param headersOut     顶层 headers（不含空行；调用方拼 From/To/Subject）
     * @param payloadOut     完整 MIME 正文（含 Content-Type 等顶层头 + 空行 + body）
     * @param errorOut       失败信息
     */
    static bool wrap(const QString& originalHeaders,
                     const QString& originalPayload,
                     const Policy& policy,
                     const QStringList& recipientEmails,
                     QString& headersOut,
                     QString& payloadOut,
                     QString* errorOut = nullptr);

    /**
     * 探测 MIME payload 是否为加密信封（S/MIME 或 口令信封）
     * @return "smime" / "password" / ""
     */
    static QString detectKind(const QString& topContentType,
                              const QString& allHeaders,
                              const QByteArray& payloadBytes);

    /**
     * 解密入口：根据 detectKind() 结果自动选解密器
     * @param mimeBytes      完整 MIME bytes（含顶层 headers）
     * @param password       口令（口令加密时必需）
     * @param errorOut
     * @return 解密后的明文 MIME；失败空 QByteArray
     */
    static QByteArray unwrap(const QByteArray& mimeBytes,
                             const QString& password,
                             QString* errorOut = nullptr);

    static QString modeName(Mode m);
};