#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

/**
 * Crypto: 密码加解密
 * 对应原 Electron 的 safeStorage（使用系统 DPAPI/CryptProtectData）
 * Windows 上使用 DPAPI (Data Protection API)
 */
class Crypto : public QObject {
    Q_OBJECT

public:
    static Crypto& instance();

    // 加密明文，返回 Base64 编码的密文
    QString encrypt(const QString& plaintext);
    // 解密 Base64 编码的密文
    QString decrypt(const QString& ciphertextBase64);
    // 是否可用
    bool isAvailable() const;

private:
    Crypto(QObject* parent = nullptr);
    Crypto(const Crypto&) = delete;
    Crypto& operator=(const Crypto&) = delete;
};
