#pragma once

#include <QString>
#include <QByteArray>

/**
 * CryptoEnvelopes: 邮件加密"信封"识别常量
 *
 * 发送端将"原始邮件 payload + headers"打包后整体加密，得到 ciphertext；
 * 然后用一个新的 multipart/alternative 把 ciphertext 包成一个 SMTP 兼容的 MIME 邮件：
 *
 *   multipart/alternative
 *     ├── text/plain           ← 人类可读的"这封邮件已加密"提示
 *     └── application/octet-stream
 *           filename="encrypted.bin"
 *           X-Mail-Encryption: password-v1     ← 自定义 header
 *           X-Mail-Encryption-Algo: AES-256-GCM  ← 加密参数
 *           Content-Transfer-Encoding: base64
 *           <base64 ciphertext>
 *
 * 接收端先看 Content-Type 是否 application/pkcs7-mime (S/MIME) 或 X-Mail-Encryption: password-v1，
 * 再决定走 SmimeCrypto 还是 PasswordCrypto 解密。
 *
 * 为什么不直接用 Content-Type: application/pkcs7-mime？
 *   - application/pkcs7-mime 是 S/MIME 专用，需要收件人有 X.509 证书才能解密
 *   - 我们自定义的"口令加密"用 application/octet-stream + X-Mail-Encryption 头，便于和 S/MIME 区分
 */
namespace CryptoEnvelopes {
    // ── 自定义 header 名 ──
    inline const QString H_ENCRYPTION    = QStringLiteral("X-Mail-Encryption");
    inline const QString H_ALGO          = QStringLiteral("X-Mail-Encryption-Algo");
    inline const QString H_FINGERPRINT   = QStringLiteral("X-Mail-Encryption-Fingerprint");
    inline const QString H_HINT          = QStringLiteral("X-Mail-Encryption-Hint");

    // ── 加密方案标识 ──
    inline const QString MODE_PASSWORD_V1 = QStringLiteral("password-v1");   // PBKDF2 + AES-256-GCM

    // ── 文件名 ──
    inline const QString ENCRYPTED_FILE_NAME = QStringLiteral("encrypted.bin");
    inline const QString SALT_FILE_NAME       = QStringLiteral("salt.bin");

    // ── 密码信封参数 (PBKDF2 + AES-256-GCM) ──
    // 用 PBKDF2-HMAC-SHA256 从口令派生 32 字节 AES key
    inline const int PBKDF2_ITERATIONS = 100'000;        // 100k 迭代（OWASP 推荐）
    inline const int PBKDF2_KEYLEN     = 32;             // AES-256
    inline const int SALT_LEN          = 16;             // 128-bit salt
    inline const int IV_LEN            = 12;             // GCM 推荐 96-bit nonce
    inline const int TAG_LEN           = 16;             // GCM 128-bit auth tag

    // ── 二进制信封格式 (密码加密明文 → ciphertext 之前再 wrap) ──
    //   magic   (4B) "MEC1"  Magic + version
    //   salt    (16B)
    //   iv      (12B)
    //   tag     (16B)
    //   data    (N B)
    inline const char   MAGIC[4]   = { 'M', 'E', 'C', '1' };

    // ── S/MIME 标识 ──
    inline const QString SMIME_TYPE_ENVELOPED =
        QStringLiteral("application/pkcs7-mime; smime-type=enveloped-data; name=\"smime.p7m\"");
    inline const QString SMIME_FILE_NAME = QStringLiteral("smime.p7m");
}