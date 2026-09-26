#include "SmimeCrypto.h"
#include "core/Logger.h"

#include <QRegularExpression>
#include <QFile>
#include <QByteArray>

#include <openssl/cms.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/objects.h>

namespace {
    QString ossErr() {
        char buf[256] = {0};
        unsigned long e = ERR_get_error();
        if (e) ERR_error_string_n(e, buf, sizeof(buf));
        return QString::fromUtf8(buf);
    }

    void setOut(QString* out, const QString& msg) {
        if (out) *out = msg;
        Logger::instance().error(QString("SmimeCrypto: %1").arg(msg), "mail");
    }

    // 把 bio 全部读出为 QByteArray
    QByteArray slurp(BIO* bio) {
        if (!bio) return {};
        QByteArray out;
        char buf[4096];
        int n;
        while ((n = BIO_read(bio, buf, sizeof(buf))) > 0) {
            out.append(buf, n);
        }
        return out;
    }
}

QByteArray SmimeCrypto::encrypt(const QByteArray& mimeBytes,
                                const QList<X509*>& recipientCerts,
                                QString* errorOut) {
    if (mimeBytes.isEmpty()) { setOut(errorOut, "明文为空"); return {}; }
    if (recipientCerts.isEmpty()) { setOut(errorOut, "收件人证书列表为空"); return {}; }

    // 准备收件人证书栈
    STACK_OF(X509)* recips = sk_X509_new_null();
    if (!recips) { setOut(errorOut, "sk_X509_new_null 失败"); return {}; }
    for (X509* x : recipientCerts) {
        if (x) sk_X509_push(recips, x);
    }
    if (sk_X509_num(recips) == 0) {
        sk_X509_free(recips);
        setOut(errorOut, "所有收件人证书都为空");
        return {};
    }

    // 明文 BIO
    BIO* inBio = BIO_new_mem_buf(mimeBytes.constData(), mimeBytes.size());
    // 输出 BIO
    BIO* outBio = BIO_new(BIO_s_mem());
    if (!inBio || !outBio) {
        if (inBio) BIO_free(inBio);
        if (outBio) BIO_free(outBio);
        sk_X509_free(recips);
        setOut(errorOut, "BIO 创建失败"); return {};
    }

    // CMS_encrypt flags:
    //   CMS_PARTIAL  - 我们手动调用 Final
    //   CMS_BINARY  - 不做 SMIME 头尾（纯 CMS）
    CMS_ContentInfo* cms = CMS_encrypt(recips, inBio, EVP_aes_256_cbc(),
                                       CMS_BINARY | CMS_PARTIAL);
    if (!cms) {
        BIO_free(inBio); BIO_free(outBio); sk_X509_free(recips);
        setOut(errorOut, "CMS_encrypt 失败: " + ossErr());
        return {};
    }
    // OpenSSL 1.1.1: CMS_final(cms, data, dcont, flags)
    if (!CMS_final(cms, inBio, nullptr, CMS_BINARY)) {
        CMS_ContentInfo_free(cms);
        BIO_free(inBio); BIO_free(outBio); sk_X509_free(recips);
        setOut(errorOut, "CMS_final 失败: " + ossErr());
        return {};
    }

    // i2d_CMS_bio: DER 编码到 BIO
    if (!i2d_CMS_bio(outBio, cms)) {
        CMS_ContentInfo_free(cms);
        BIO_free(inBio); BIO_free(outBio); sk_X509_free(recips);
        setOut(errorOut, "i2d_CMS_bio 失败: " + ossErr());
        return {};
    }
    QByteArray der = slurp(outBio);

    CMS_ContentInfo_free(cms);
    BIO_free(inBio);
    BIO_free(outBio);
    sk_X509_free(recips);
    return der;
}

// 单收件人便捷重载（保留向后兼容）
QByteArray SmimeCrypto::encrypt(const QByteArray& mimeBytes,
                                X509* recipientCert,
                                QString* errorOut) {
    QList<X509*> one;
    if (recipientCert) one.append(recipientCert);
    return encrypt(mimeBytes, one, errorOut);
}

QByteArray SmimeCrypto::decrypt(const QByteArray& derEnvelope,
                                EVP_PKEY* privateKey, X509* myCert,
                                QString* errorOut) {
    if (derEnvelope.isEmpty()) { setOut(errorOut, "信封为空"); return {}; }
    if (!privateKey || !myCert) { setOut(errorOut, "私钥/证书为空"); return {}; }

    BIO* inBio = BIO_new_mem_buf(derEnvelope.constData(), derEnvelope.size());
    BIO* outBio = BIO_new(BIO_s_mem());
    if (!inBio || !outBio) {
        if (inBio) BIO_free(inBio);
        if (outBio) BIO_free(outBio);
        setOut(errorOut, "BIO 创建失败");
        return {};
    }

    CMS_ContentInfo* cms = d2i_CMS_bio(inBio, nullptr);
    if (!cms) {
        BIO_free(inBio); BIO_free(outBio);
        setOut(errorOut, "d2i_CMS_bio 失败: " + ossErr());
        return {};
    }
    if (!CMS_decrypt_set1_pkey(cms, privateKey, myCert)) {
        CMS_ContentInfo_free(cms); BIO_free(inBio); BIO_free(outBio);
        setOut(errorOut, "CMS_decrypt_set1_pkey 失败: " + ossErr());
        return {};
    }
    // OpenSSL 1.1.1: CMS_decrypt(cms, pkey, cert, dcont, out, flags)
    if (!CMS_decrypt(cms, nullptr, nullptr, nullptr, outBio, 0)) {
        CMS_ContentInfo_free(cms); BIO_free(inBio); BIO_free(outBio);
        setOut(errorOut, "CMS_decrypt 失败（无匹配私钥/证书）: " + ossErr());
        return {};
    }

    QByteArray mime = slurp(outBio);
    CMS_ContentInfo_free(cms);
    BIO_free(inBio);
    BIO_free(outBio);
    return mime;
}

bool SmimeCrypto::isSmimeEnvelope(const QByteArray& data) {
    if (data.isEmpty()) return false;
    BIO* inBio = BIO_new_mem_buf(data.constData(), data.size());
    CMS_ContentInfo* cms = d2i_CMS_bio(inBio, nullptr);
    BIO_free(inBio);
    if (!cms) return false;
    // 检查是否是 EnvelopedData
    const ASN1_OBJECT* ctype = CMS_get0_type(cms);
    bool isEnveloped = (ctype &&
        OBJ_obj2nid(ctype) == NID_pkcs7_enveloped);
    CMS_ContentInfo_free(cms);
    return isEnveloped;
}

SmimeCrypto::EnvelopeMeta SmimeCrypto::peekMeta(const QByteArray& derEnvelope) {
    EnvelopeMeta meta;
    if (derEnvelope.isEmpty()) return meta;
    BIO* inBio = BIO_new_mem_buf(derEnvelope.constData(), derEnvelope.size());
    CMS_ContentInfo* cms = d2i_CMS_bio(inBio, nullptr);
    BIO_free(inBio);
    if (!cms) return meta;

    const ASN1_OBJECT* ctype = CMS_get0_type(cms);
    if (ctype && OBJ_obj2nid(ctype) == NID_pkcs7_enveloped) {
        STACK_OF(CMS_RecipientInfo)* ris = CMS_get0_RecipientInfos(cms);
        int n = (ris ? sk_CMS_RecipientInfo_num(ris) : 0);
        meta.recipientIssuer = QStringLiteral("(S/MIME, %1 个收件人)").arg(n);
        meta.keyCipher = -1;
    }
    CMS_ContentInfo_free(cms);
    return meta;
}

// ─────────────────────────────────────────────────────────────────────────────
// S/MIME 签名 (RFC 5652 SignedData, detached)
// ─────────────────────────────────────────────────────────────────────────────

QByteArray SmimeCrypto::sign(const QByteArray& mimeBytes,
                             X509* signerCert,
                             EVP_PKEY* signerKey,
                             QString* errorOut) {
    if (mimeBytes.isEmpty())   { setOut(errorOut, "明文 MIME 为空"); return {}; }
    if (!signerCert)           { setOut(errorOut, "签名者证书为空"); return {}; }
    if (!signerKey)            { setOut(errorOut, "签名者私钥为空"); return {}; }

    BIO* inBio  = BIO_new_mem_buf(mimeBytes.constData(), mimeBytes.size());
    BIO* outBio = BIO_new(BIO_s_mem());
    if (!inBio || !outBio) {
        if (inBio) BIO_free(inBio);
        if (outBio) BIO_free(outBio);
        setOut(errorOut, "BIO 创建失败");
        return {};
    }

    // CMS_sign flags:
    //   CMS_DETACHED     - detached signature（签名块不含原数据）
    //   CMS_BINARY       - 不做 SMIME 头尾
    //   CMS_PARTIAL      - 手动 Final
    //   CMS_NOCERTS      - 不在签名中嵌入证书（接收方有公钥可独立验证；省略节省 1KB+）
    // 我们选不嵌证书（接收方拿到发件人证书就行；签名块保持小体积）。
    CMS_ContentInfo* cms = CMS_sign(nullptr, nullptr, nullptr, inBio,
                                   CMS_DETACHED | CMS_BINARY | CMS_PARTIAL | CMS_NOCERTS);
    if (!cms) {
        BIO_free(inBio); BIO_free(outBio);
        setOut(errorOut, "CMS_sign 失败: " + ossErr());
        return {};
    }
    // 加入签名者
    if (!CMS_add1_signer(cms, signerCert, signerKey, EVP_sha256(), CMS_BINARY)) {
        CMS_ContentInfo_free(cms);
        BIO_free(inBio); BIO_free(outBio);
        setOut(errorOut, "CMS_add1_signer 失败: " + ossErr());
        return {};
    }
    // 完成
    if (!CMS_final(cms, inBio, nullptr, CMS_BINARY)) {
        CMS_ContentInfo_free(cms);
        BIO_free(inBio); BIO_free(outBio);
        setOut(errorOut, "CMS_final(sign) 失败: " + ossErr());
        return {};
    }
    // DER 输出
    if (!i2d_CMS_bio(outBio, cms)) {
        CMS_ContentInfo_free(cms);
        BIO_free(inBio); BIO_free(outBio);
        setOut(errorOut, "i2d_CMS_bio(sign) 失败: " + ossErr());
        return {};
    }

    QByteArray der = slurp(outBio);
    CMS_ContentInfo_free(cms);
    BIO_free(inBio);
    BIO_free(outBio);
    return der;
}

// ─────────────────────────────────────────────────────────────────────────────
// 验证 multipart/signed 的 detached signature
// ─────────────────────────────────────────────────────────────────────────────

SmimeCrypto::VerifyResult SmimeCrypto::verify(const QByteArray& mimeBytes,
                                             const QString& trustDirPem,
                                             QString* errorOut) {
    VerifyResult res;
    if (mimeBytes.isEmpty()) {
        res.errorReason = "邮件字节为空";
        if (errorOut) *errorOut = res.errorReason;
        return res;
    }

    // 1) 找顶层 headers/body 分隔
    int hdrEnd = mimeBytes.indexOf("\r\n\r\n");
    if (hdrEnd < 0) hdrEnd = mimeBytes.indexOf("\n\n");
    if (hdrEnd < 0) {
        res.errorReason = "找不到 headers/body 分隔";
        if (errorOut) *errorOut = res.errorReason;
        return res;
    }
    int bodyStart = hdrEnd + (mimeBytes.mid(hdrEnd, 4) == "\r\n\r\n" ? 4 : 2);
    QByteArray headers = mimeBytes.left(hdrEnd);
    QByteArray body    = mimeBytes.mid(bodyStart);

    // 2) 找 Content-Type 头
    QString topCtype;
    QRegularExpression ctypeRe("^Content-Type:[ \\t]*([^;\\r\\n]+)",
                               QRegularExpression::CaseInsensitiveOption |
                               QRegularExpression::MultilineOption);
    auto cm = ctypeRe.match(QString::fromLatin1(headers));
    if (cm.hasMatch()) topCtype = cm.captured(1).trimmed().toLower();
    if (!topCtype.contains("multipart/signed")) {
        res.errorReason = "不是 multipart/signed";
        if (errorOut) *errorOut = res.errorReason;
        return res;
    }

    // 3) 取 boundary
    QRegularExpression bndRe("boundary=\"?([^\\s\"]+)\"?",
                             QRegularExpression::CaseInsensitiveOption);
    auto bm = bndRe.match(QString::fromLatin1(headers));
    if (!bm.hasMatch()) {
        res.errorReason = "找不到 boundary";
        if (errorOut) *errorOut = res.errorReason;
        return res;
    }
    QString bnd = bm.captured(1);

    // 4) 按 boundary 切分 part
    QRegularExpression partRe(QStringLiteral("\\r?\\n--%1(?:\\r?\\n|--)").arg(QRegularExpression::escape(bnd)));
    QString bodyStr = QString::fromLatin1(body);
    QStringList parts = bodyStr.split(partRe, Qt::KeepEmptyParts);
    // parts[0] 通常为空
    if (parts.size() < 3) {
        res.errorReason = "multipart/signed part 数量不足";
        if (errorOut) *errorOut = res.errorReason;
        return res;
    }

    // 5) 第一 part = 原 MIME（含完整 headers + body）
    // 第二 part = 签名 (application/pkcs7-signature)
    QByteArray originalPart = parts[1].toUtf8();
    QByteArray signaturePart = parts[2].toUtf8();

    // 签名 part 提取 base64 body
    int sigHdrEnd = signaturePart.indexOf("\r\n\r\n");
    if (sigHdrEnd < 0) sigHdrEnd = signaturePart.indexOf("\n\n");
    if (sigHdrEnd < 0) {
        res.errorReason = "签名 part 找不到 body";
        if (errorOut) *errorOut = res.errorReason;
        return res;
    }
    int sigBodyStart = sigHdrEnd + (signaturePart.mid(sigHdrEnd, 4) == "\r\n\r\n" ? 4 : 2);
    QByteArray sigB64 = signaturePart.mid(sigBodyStart);
    sigB64.replace("\r", "").replace("\n", "");
    QByteArray sigDer = QByteArray::fromBase64(sigB64);
    if (sigDer.isEmpty()) {
        res.errorReason = "签名 base64 解码失败";
        if (errorOut) *errorOut = res.errorReason;
        return res;
    }

    // 把签名 part 的原始 MIME 保留下来（用于 UI 展示）
    res.signatureMime = QString::fromUtf8(signaturePart).left(800);  // 截断展示

    // 6) CMS_verify
    BIO* inBio  = BIO_new_mem_buf(sigDer.constData(), sigDer.size());
    BIO* origBio = BIO_new_mem_buf(originalPart.constData(), originalPart.size());
    BIO* certOutBio = BIO_new(BIO_s_mem());   // 验证过程中提取签名者证书
    if (!inBio || !origBio || !certOutBio) {
        if (inBio) BIO_free(inBio);
        if (origBio) BIO_free(origBio);
        if (certOutBio) BIO_free(certOutBio);
        res.errorReason = "BIO 创建失败";
        if (errorOut) *errorOut = res.errorReason;
        return res;
    }

    // 加载可选的信任根
    X509_STORE* store = X509_STORE_new();
    if (!trustDirPem.isEmpty()) {
        QFile tf(trustDirPem);
        if (tf.open(QIODevice::ReadOnly)) {
            QByteArray pem = tf.readAll();
            tf.close();
            BIO* pb = BIO_new_mem_buf(pem.constData(), pem.size());
            if (pb) {
                while (true) {
                    X509* x = PEM_read_bio_X509(pb, nullptr, nullptr, nullptr);
                    if (!x) break;
                    X509_STORE_add_cert(store, x);
                    X509_free(x);
                }
                BIO_free(pb);
            }
        }
    }

    CMS_ContentInfo* cms = d2i_CMS_bio(inBio, nullptr);
    if (!cms) {
        X509_STORE_free(store);
        BIO_free(inBio); BIO_free(origBio); BIO_free(certOutBio);
        res.errorReason = "d2i_CMS_bio(签名) 失败";
        if (errorOut) *errorOut = res.errorReason;
        return res;
    }

    // CMS_verify 标志：
    //   CMS_DETACHED  - 我们手动提供原数据 BIO
    //   CMS_BINARY    - 不做 SMIME 头尾
    //   CMS_NOVERIFY  - 跳过证书链校验（仅验证签名哈希）—— 这里我们想做完整校验，
    //                   所以不设 NOVERIFY，让 OpenSSL 用 store 校验链
    int vflags = CMS_DETACHED | CMS_BINARY;
    int rc = CMS_verify(cms, nullptr, store, origBio, certOutBio, vflags);
    if (rc <= 0) {
        res.ok = false;
        res.errorReason = QStringLiteral("CMS_verify 失败: %1").arg(ossErr());
        CMS_ContentInfo_free(cms);
        X509_STORE_free(store);
        BIO_free(inBio); BIO_free(origBio); BIO_free(certOutBio);
        if (errorOut) *errorOut = res.errorReason;
        return res;
    }

    // 提取签名者证书
    QByteArray certBytes = slurp(certOutBio);
    X509* signer = nullptr;
    if (!certBytes.isEmpty()) {
        BIO* cb = BIO_new_mem_buf(certBytes.constData(), certBytes.size());
        signer = PEM_read_bio_X509(cb, nullptr, nullptr, nullptr);
        BIO_free(cb);
    }
    if (signer) {
        char buf[256] = {0};
        X509_NAME_get_text_by_NID(X509_get_subject_name(signer), NID_commonName, buf, sizeof(buf));
        res.signerSubject = QString::fromUtf8(buf);

        // 提取 email (SAN 或 emailAddress)
        STACK_OF(GENERAL_NAME)* names = static_cast<STACK_OF(GENERAL_NAME)*>(
            X509_get_ext_d2i(signer, NID_subject_alt_name, nullptr, nullptr));
        if (names) {
            for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
                GENERAL_NAME* gn = sk_GENERAL_NAME_value(names, i);
                int type = 0;
                const ASN1_STRING* str =
                    static_cast<const ASN1_STRING*>(GENERAL_NAME_get0_value(gn, &type));
                if (type == GEN_EMAIL && str) {
                    res.signerEmail = QString::fromUtf8(
                        reinterpret_cast<const char*>(ASN1_STRING_get0_data(str)),
                        ASN1_STRING_length(str));
                    break;
                }
            }
            GENERAL_NAMES_free(names);
        }
        if (res.signerEmail.isEmpty()) {
            X509_NAME* subj = X509_get_subject_name(signer);
            int idx = X509_NAME_get_index_by_NID(subj, NID_pkcs9_emailAddress, -1);
            if (idx >= 0) {
                X509_NAME_ENTRY* e = X509_NAME_get_entry(subj, idx);
                if (e) {
                    ASN1_STRING* str = X509_NAME_ENTRY_get_data(e);
                    if (str) {
                        res.signerEmail = QString::fromUtf8(
                            reinterpret_cast<const char*>(ASN1_STRING_get0_data(str)),
                            ASN1_STRING_length(str));
                    }
                }
            }
        }
        X509_free(signer);
    } else {
        // 签名块里没证书 —— 验证的是签名哈希但不知道是谁签的
        res.errorReason = "签名验证通过但签名块未含证书";
    }
    res.ok = true;
    res.digestAlg = "sha-256";

    CMS_ContentInfo_free(cms);
    X509_STORE_free(store);
    BIO_free(inBio); BIO_free(origBio); BIO_free(certOutBio);
    return res;
}

bool SmimeCrypto::isSigned(const QByteArray& mimeBytes) {
    if (mimeBytes.isEmpty()) return false;
    QByteArray headers;
    int hdrEnd = mimeBytes.indexOf("\r\n\r\n");
    if (hdrEnd < 0) hdrEnd = mimeBytes.indexOf("\n\n");
    if (hdrEnd < 0) return false;
    headers = mimeBytes.left(hdrEnd);

    QRegularExpression ctypeRe("^Content-Type:[ \\t]*multipart/signed",
                               QRegularExpression::CaseInsensitiveOption |
                               QRegularExpression::MultilineOption);
    return ctypeRe.match(QString::fromLatin1(headers)).hasMatch();
}