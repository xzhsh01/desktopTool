#pragma once

#include <QString>
#include <QByteArray>
#include <QList>

typedef struct x509_st X509;
typedef struct evp_pkey_st EVP_PKEY;

/**
 * SmimeCrypto: S/MIME 加密 / 解密
 *
 * 加密：输入明文 MIME bytes + 收件人 X.509 公钥 → DER 编码的 CMS EnvelopedData
 *       （RFC 5652 / RFC 8551）
 *
 * 解密：输入 DER 编码的 CMS EnvelopedData + 我的私钥 + 我的证书 → 明文 MIME bytes
 *
 * 输出格式：
 *   - 加密侧用二进制 DER（不是 PEM），便于嵌入到 SMTP 的 application/pkcs7-mime part
 *   - 解密侧接受任意 CMS 容器（DER 或 PEM）
 */
class SmimeCrypto {
public:
    /**
     * 加密：明文 MIME bytes → DER 编码的 CMS EnvelopedData
     *
     * 支持 1 个或多个收件人（CMS EnvelopedData 的 RecipientInfo 列表）。
     * 当收件人 > 1 时，每个收件人都能用自己私钥解开同一密文。
     *
     * @param mimeBytes      整封邮件 MIME 原文（含 headers + body）
     * @param recipientCerts 收件人 X.509* 列表（至少 1 个；所有权仍归调用方）
     * @param errorOut       失败信息
     * @return 加密后的 DER bytes；失败返回空 QByteArray
     */
    static QByteArray encrypt(const QByteArray& mimeBytes,
                              const QList<X509*>& recipientCerts,
                              QString* errorOut = nullptr);

    /**
     * 单收件人便捷重载（向后兼容）
     */
    static QByteArray encrypt(const QByteArray& mimeBytes,
                              X509* recipientCert,
                              QString* errorOut = nullptr);

    // ── S/MIME 签名（multipart/signed 用的 detached signature） ──

    /**
     * 对 MIME 明文做 detached signature（RFC 5652 SignedData）
     *
     * @param mimeBytes      完整的待签名 MIME bytes（含顶层 headers + body）
     * @param signerCert     签名者证书（含公钥）
     * @param signerKey      签名者私钥
     * @param errorOut       失败信息
     * @return DER 编码的 CMS SignedData（仅签名块，不含原 MIME）；失败空
     */
    static QByteArray sign(const QByteArray& mimeBytes,
                           X509* signerCert,
                           EVP_PKEY* signerKey,
                           QString* errorOut = nullptr);

    /**
     * 验证 multipart/signed 中的 detached signature
     *
     * @param mimeBytes      完整的 multipart/signed 邮件 bytes（含顶层 headers + body）
     * @param errorOut       失败信息
     * @return 验证结果：signerSubject + 验证状态（valid/invalid/no证书/no签名）
     */
    struct VerifyResult {
        bool    ok              = false;       // 签名验证通过
        QString signerSubject;                  // 签名者主题 (CN)
        QString signerEmail;                   // 签名者邮箱 (SAN)
        QString digestAlg;                     // sha-256 等
        QString errorReason;                   // 失败原因（ok=false 时填）
        QString signatureMime;                 // multipart/signed 中签名 part 的原始 MIME（含 headers + base64）
    };
    static VerifyResult verify(const QByteArray& mimeBytes,
                               const QString& trustDirPem,   // 可选：信任根 PEM 文件路径；空则不验证证书链
                               QString* errorOut = nullptr);

    /**
     * 探测 MIME 是否为 multipart/signed（带 application/pkcs7-signature）
     * @return true / false
     */
    static bool isSigned(const QByteArray& mimeBytes);

    /**
     * 解密：DER/PEM 编码的 CMS EnvelopedData → 明文 MIME bytes
     *
     * @param derEnvelope   加密的 CMS 字节流（DER 或 PEM 均可，自动嗅探）
     * @param privateKey    我的私钥（EVP_PKEY*，由 CertificateManager 解锁提供）
     * @param myCert        我的证书（含公钥，私钥匹配用）
     * @param errorOut      失败信息
     * @return 解密后的明文 MIME；失败返回空 QByteArray
     */
    static QByteArray decrypt(const QByteArray& derEnvelope,
                              EVP_PKEY* privateKey,
                              X509* myCert,
                              QString* errorOut = nullptr);

    /**
     * 嗅探：给定字节流是否为 S/MIME CMS EnvelopedData 或 SignedData
     * 通过 OpenSSL d2i CMS_ContentInfo 试解析判断
     */
    static bool isSmimeEnvelope(const QByteArray& data);

    /**
     * 取信封的发件人 / 收件人 / 颁发者信息（仅元数据，不解密）
     *   - 取 CMS EnvelopedData 的 RecipientInfo → Issuer/SerialNumber
     *   - 用于在解密前显示"这封加密邮件是发给谁的"
     */
    struct EnvelopeMeta {
        QString recipientIssuer;     // 颁发者 CN
        QString recipientEmailHint;  // 收件人邮箱 hint（若有 SAN）
        int     keyCipher = -1;      // NID_aes_256_cbc 等
    };
    static EnvelopeMeta peekMeta(const QByteArray& derEnvelope);
};