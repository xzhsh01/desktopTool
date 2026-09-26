#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>

// 前向声明 OpenSSL 类型，避免在头文件中暴露 openssl/x509.h
typedef struct x509_st X509;
typedef struct evp_pkey_st EVP_PKEY;

/**
 * CertificateManager: X.509 证书 / 私钥本地仓库
 *
 * 用途：邮件 S/MIME 加密的"证书基础设施"
 *   - "我"的个人证书（含私钥）：用于解密别人发给我的加密邮件 / 签名我的邮件
 *   - "联系人"的公钥证书：用于给对方发加密邮件
 *
 * 存储：
 *   %APPDATA%/KFrame/bambooRat/mail/certs/
 *     ├── personal/<id>.p12          ← 个人证书 (PKCS#12, 私钥+证书)
 *     ├── contacts/<sha1fp>.pem      ← 联系人公钥 (PEM X.509)
 *     └── index.json                 ← 个人证书元数据（CN / Email / 过期时间）
 *
 * PKCS#12 文件本身已用密码加密；额外再用 core/Crypto::encrypt (DPAPI) 二次保护
 * —— 防止他人拷走 .p12 后暴力破解弱口令。
 */
class CertificateManager : public QObject {
    Q_OBJECT
public:
    struct CertInfo {
        QString id;              // personal: 文件名（不含 .p12）；contact: sha1fp
        bool    isPersonal = false;  // 是否含私钥
        QString commonName;      // CN
        QString email;           // Subject Alternative Name: email
        QString issuer;          // 颁发者 CN
        QString sha1Fingerprint; // SHA-1 指纹（hex 大写，无冒号）
        QDateTime notBefore;
        QDateTime notAfter;
        QString sourcePath;      // 磁盘文件路径
    };

    static CertificateManager& instance();

    // ── 个人证书（私钥） ──
    // 从 .p12/.pfx 导入个人证书；password 为 PKCS#12 口令
    // 成功返回 CertInfo（含 id 字段），失败 CertInfo.id 为空 + getLastError()
    CertInfo importPersonal(const QString& p12Path, const QString& password);

    // 列出所有个人证书（含私钥）
    QList<CertInfo> listPersonal() const;

    // 删除个人证书（按 id）
    bool removePersonal(const QString& id);

    // 解锁个人证书私钥（用于 SmimeCrypto 解密）；用户输入口令
    // 返回的 EVP_PKEY* 调用方负责 EVP_PKEY_free()；失败 nullptr
    EVP_PKEY* unlockPersonalKey(const QString& id, const QString& password) const;

    // 获取个人证书的 X509*（不含私钥）；失败 nullptr
    X509* getPersonalCert(const QString& id) const;

    // 导出个人证书到 PKCS#12 (.p12)
    //   - newP12Password: 新 .p12 文件的口令（用户自设）
    //   - originalPassword: 解锁本地存储的 .p12 所需口令（用现有 PKCS#12 口令）
    // 失败时 errorOut 含原因
    bool exportPersonal(const QString& id,
                        const QString& destPath,
                        const QString& originalPassword,
                        const QString& newP12Password,
                        QString* errorOut = nullptr);

    // ── 联系人公钥 ──
    // 从 .pem/.crt 导入联系人公钥
    CertInfo importContact(const QString& pemPath);

    // 按 email 查找联系人公钥（精确匹配，大小写不敏感）
    CertInfo findByEmail(const QString& email) const;

    // 列出所有联系人证书
    QList<CertInfo> listContacts() const;

    bool removeContact(const QString& id);

    // 导出联系人证书到目标 PEM 文件（拷贝现有 PEM 副本，方便分享/重分发）
    // 同时保存到剪贴板一份（用户可粘贴给同事）
    bool exportContact(const QString& id, const QString& destPemPath, QString* errorOut = nullptr);

    // 把联系人证书 PEM 内容读回（用于剪贴板/预览）
    QString readContactPem(const QString& id) const;

    // ── 工具 ──
    static QString getLastError();
    static void    clearLastError();

    // 证书目录（%APPDATA%/KFrame/bambooRat/mail/certs/）
    static QString certsDir();
    static QString personalDir();
    static QString contactsDir();
    static QString indexPath();

signals:
    void certAdded(const CertInfo& info);
    void certRemoved(const QString& id);

private:
    CertificateManager(QObject* parent = nullptr);
    CertificateManager(const CertificateManager&) = delete;
    CertificateManager& operator=(const CertificateManager&) = delete;

    // 把 CertInfo 持久化到 index.json
    void saveIndex() const;
    void loadIndex();   // 启动时调用一次

    struct IndexData;
    IndexData* m_idx = nullptr; // PIMPL 简化
};