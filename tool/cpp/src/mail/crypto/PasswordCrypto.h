#pragma once

#include <QString>
#include <QByteArray>

/**
 * PasswordCrypto: 基于口令的对称加密（AES-256-GCM）
 *
 * 密钥派生：PBKDF2-HMAC-SHA256(password, salt, iterations) -> 32B AES key
 * 数据加密：AES-256-GCM(key, iv, plaintext) -> ciphertext + tag
 *
 * 用途：
 *   - 邮件加密的"口令方案"（收件人无 X.509 证书时的回退）
 *   - 邮箱密码本地缓存的二次加密（可选，配合 core/Crypto 的 DPAPI）
 *
 * 不持久化口令；调用方负责传递明文口令。
 */
class PasswordCrypto {
public:
    /**
     * 用口令加密明文，返回二进制信封：
     *   magic(4B "MEC1") + salt(16B) + iv(12B) + tag(16B) + ciphertext(N)
     */
    static QByteArray encrypt(const QString& password, const QByteArray& plaintext);

    /**
     * 用口令解密密文信封
     * @return 解密后的明文；失败返回空 QByteArray（错误查看 getLastError()）
     */
    static QByteArray decrypt(const QString& password, const QByteArray& envelope);

    /**
     * 计算 envelope 的 SHA-256 fingerprint（前 8 字节 hex），用于 UI 显示
     *   "加密指纹 3A2F4B1C..."，让用户在输入口令前能先核对是不是这封邮件
     */
    static QString fingerprint(const QByteArray& envelope);

    /**
     * 校验 envelope 魔数是否匹配 (MEC1)，用于接收端快速识别
     */
    static bool isPasswordEnvelope(const QByteArray& data);

    static QString getLastError();
    static void    clearLastError();
};