#include "CertificateManager.h"
#include "CryptoEnvelopes.h"

#include "core/Crypto.h"
#include "core/Logger.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs12.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/sha.h>

namespace {
    QString s_lastError;

    void setErr(const QString& e) {
        s_lastError = e;
        Logger::instance().error(QString("CertificateManager: %1").arg(e), "mail");
    }

    QString sha1Hex(X509* x509) {
        if (!x509) return {};
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int  mdlen = 0;
        if (!X509_digest(x509, EVP_sha1(), md, &mdlen)) return {};
        return QByteArray(reinterpret_cast<char*>(md), static_cast<int>(mdlen)).toHex().toUpper();
    }

    QString extractEmail(X509* x509) {
        if (!x509) return {};
        // OpenSSL 1.1.1+ : GENERAL_NAME 是 opaque; 不能直接访问 union
        // 用 OpenSSL 提供的 GENERAL_NAME_get0_value + GEN_EMAIL 类型判断
        STACK_OF(GENERAL_NAME)* names = static_cast<STACK_OF(GENERAL_NAME)*>(
            X509_get_ext_d2i(x509, NID_subject_alt_name, nullptr, nullptr));
        if (names) {
            for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
                GENERAL_NAME* gn = sk_GENERAL_NAME_value(names, i);
                int type = 0;
                // OpenSSL 1.1.1 老版本 GENERAL_NAME_get0_value 返回 void*，
                // 新版本返回 ASN1_STRING*。统一用 const ASN1_STRING* 接收。
                const ASN1_STRING* str =
                    static_cast<const ASN1_STRING*>(GENERAL_NAME_get0_value(gn, &type));
                if (type == GEN_EMAIL && str) {
                    QString email = QString::fromUtf8(
                        reinterpret_cast<const char*>(ASN1_STRING_get0_data(str)),
                        ASN1_STRING_length(str));
                    GENERAL_NAMES_free(names);
                    return email;
                }
            }
            GENERAL_NAMES_free(names);
        }
        // 退回：从 Subject DN 找 emailAddress= 属性 (OID 1.2.840.113549.1.9.1 / NID_pkcs9_emailAddress)
        X509_NAME* subj = X509_get_subject_name(x509);
        if (subj) {
            int idx = X509_NAME_get_index_by_NID(subj, NID_pkcs9_emailAddress, -1);
            if (idx >= 0) {
                X509_NAME_ENTRY* e = X509_NAME_get_entry(subj, idx);
                if (e) {
                    ASN1_STRING* str = X509_NAME_ENTRY_get_data(e);
                    if (str) {
                        return QString::fromUtf8(
                            reinterpret_cast<const char*>(ASN1_STRING_get0_data(str)),
                            ASN1_STRING_length(str));
                    }
                }
            }
        }
        return {};
    }

    QString extractNameByNid(X509* x509, int nid) {
        if (!x509) return {};
        X509_NAME* nm = X509_get_subject_name(x509);
        if (!nm) return {};
        char buf[256] = {0};
        X509_NAME_get_text_by_NID(nm, nid, buf, sizeof(buf));
        return QString::fromUtf8(buf);
    }

    // OpenSSL error stack → string
    QString opensslErrStr() {
        char buf[256] = {0};
        unsigned long e = ERR_get_error();
        if (e) ERR_error_string_n(e, buf, sizeof(buf));
        return QString::fromUtf8(buf);
    }

    QJsonObject certInfoToJson(const CertificateManager::CertInfo& c) {
        QJsonObject o;
        o["id"]                = c.id;
        o["isPersonal"]        = c.isPersonal;
        o["commonName"]        = c.commonName;
        o["email"]             = c.email;
        o["issuer"]            = c.issuer;
        o["sha1Fingerprint"]   = c.sha1Fingerprint;
        o["notBefore"]         = c.notBefore.toString(Qt::ISODate);
        o["notAfter"]          = c.notAfter.toString(Qt::ISODate);
        return o;
    }

    CertificateManager::CertInfo certInfoFromJson(const QJsonObject& o) {
        CertificateManager::CertInfo c;
        c.id              = o["id"].toString();
        c.isPersonal      = o["isPersonal"].toBool();
        c.commonName      = o["commonName"].toString();
        c.email           = o["email"].toString();
        c.issuer          = o["issuer"].toString();
        c.sha1Fingerprint = o["sha1Fingerprint"].toString();
        c.notBefore       = QDateTime::fromString(o["notBefore"].toString(), Qt::ISODate);
        c.notAfter        = QDateTime::fromString(o["notAfter"].toString(), Qt::ISODate);
        return c;
    }
}

struct CertificateManager::IndexData {
    QJsonObject personal;  // id → CertInfo JSON
    QJsonObject contacts;  // sha1fp → CertInfo JSON
};

CertificateManager& CertificateManager::instance() {
    static CertificateManager m;
    return m;
}

CertificateManager::CertificateManager(QObject* parent) : QObject(parent) {
    loadIndex();
}

QString CertificateManager::getLastError()   { return s_lastError; }
void    CertificateManager::clearLastError() { s_lastError.clear(); }

QString CertificateManager::certsDir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base + "/mail/certs";
}

QString CertificateManager::personalDir() { return certsDir() + "/personal"; }
QString CertificateManager::contactsDir() { return certsDir() + "/contacts"; }

QString CertificateManager::indexPath() {
    return certsDir() + "/index.json";
}

void CertificateManager::loadIndex() {
    QFile f(indexPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return;
    if (!m_idx) m_idx = new IndexData;
    m_idx->personal = doc.object()["personal"].toObject();
    m_idx->contacts = doc.object()["contacts"].toObject();
}

void CertificateManager::saveIndex() const {
    if (!m_idx) return;
    QDir().mkpath(certsDir());
    QJsonObject root;
    root["personal"] = m_idx->personal;
    root["contacts"] = m_idx->contacts;
    QFile f(indexPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        f.close();
    }
}

CertificateManager::CertInfo CertificateManager::importPersonal(const QString& p12Path, const QString& password) {
    clearLastError();
    QFile f(p12Path);
    if (!f.open(QIODevice::ReadOnly)) { setErr(QString("无法打开文件: %1").arg(p12Path)); return {}; }
    QByteArray der = f.readAll();
    f.close();

    const uchar* p = reinterpret_cast<const uchar*>(der.constData());
    PKCS12* p12 = d2i_PKCS12(nullptr, &p, der.size());
    if (!p12) { setErr("d2i_PKCS12 失败: " + opensslErrStr()); return {}; }

    EVP_PKEY* pkey = nullptr;
    X509*      cert = nullptr;
    STACK_OF(X509)* ca = nullptr;

    if (!PKCS12_parse(p12, password.toUtf8().constData(), &pkey, &cert, &ca)) {
        PKCS12_free(p12);
        setErr("PKCS12_parse 失败（口令错误或文件损坏）: " + opensslErrStr());
        return {};
    }
    PKCS12_free(p12);
    if (!cert || !pkey) {
        if (pkey) EVP_PKEY_free(pkey);
        if (cert) X509_free(cert);
        if (ca)   sk_X509_pop_free(ca, X509_free);
        setErr("PKCS12 不含证书或私钥");
        return {};
    }

    CertInfo info;
    info.commonName      = extractNameByNid(cert, NID_commonName);
    info.email           = extractEmail(cert);
    info.issuer          = extractNameByNid(cert, NID_commonName); // X509_NAME_get_issuer 同名？正确做法应 X509_get_issuer_name
    info.sha1Fingerprint = sha1Hex(cert);

    // 颁发者
    X509_NAME* issuerName = X509_get_issuer_name(cert);
    if (issuerName) {
        char buf[256] = {0};
        X509_NAME_get_text_by_NID(issuerName, NID_commonName, buf, sizeof(buf));
        info.issuer = QString::fromUtf8(buf);
    }

    ASN1_TIME* nb = X509_getm_notBefore(cert);
    ASN1_TIME* na = X509_getm_notAfter(cert);
    if (nb) info.notBefore = QDateTime::fromString(QString::fromUtf8(reinterpret_cast<char*>(nb->data)), Qt::ISODate);
    if (na) info.notAfter  = QDateTime::fromString(QString::fromUtf8(reinterpret_cast<char*>(na->data)), Qt::ISODate);

    // 暂存 pkey 与 cert 准备输出 PKCS#12
    QString newId = QUuid::createUuid().toString(QUuid::WithoutBraces).left(16);
    info.id          = newId;
    info.isPersonal  = true;
    info.sourcePath  = personalDir() + "/" + newId + ".p12";

    QDir().mkpath(personalDir());

    // 用 PKCS12_create 重新打包（统一加密算法 + mac）
    // OpenSSL 1.1.1: PKCS12_create(pass, name, pkey, cert, ca, nid_key, nid_cert, iter, mac_iter, keytype)
    PKCS12* p12out = PKCS12_create(password.toUtf8().constData(),
                                   "bambooRat",
                                   pkey, cert, ca,
                                   NID_aes_256_cbc, NID_aes_256_cbc,
                                   PKCS12_DEFAULT_ITER, PKCS12_DEFAULT_ITER, 0);
    if (!p12out) {
        EVP_PKEY_free(pkey); X509_free(cert); if (ca) sk_X509_pop_free(ca, X509_free);
        setErr("PKCS12_create 失败: " + opensslErrStr());
        return {};
    }
    // 写文件
    BIO* bio = BIO_new_file(info.sourcePath.toUtf8().constData(), "wb");
    if (!bio || !i2d_PKCS12_bio(bio, p12out)) {
        if (bio) BIO_free(bio);
        PKCS12_free(p12out);
        EVP_PKEY_free(pkey); X509_free(cert); if (ca) sk_X509_pop_free(ca, X509_free);
        setErr("写 PKCS12 失败: " + opensslErrStr());
        return {};
    }
    BIO_free(bio);
    PKCS12_free(p12out);

    EVP_PKEY_free(pkey);
    X509_free(cert);
    if (ca) sk_X509_pop_free(ca, X509_free);

    // 更新 index
    if (!m_idx) m_idx = new IndexData;
    m_idx->personal.insert(newId, certInfoToJson(info));
    saveIndex();

    emit certAdded(info);
    return info;
}

QList<CertificateManager::CertInfo> CertificateManager::listPersonal() const {
    QList<CertInfo> out;
    if (!m_idx) return out;
    for (auto it = m_idx->personal.begin(); it != m_idx->personal.end(); ++it) {
        out.append(certInfoFromJson(it.value().toObject()));
    }
    return out;
}

bool CertificateManager::removePersonal(const QString& id) {
    if (!m_idx) return false;
    if (!m_idx->personal.contains(id)) return false;
    QString path = personalDir() + "/" + id + ".p12";
    QFile::remove(path);
    m_idx->personal.remove(id);
    saveIndex();
    emit certRemoved(id);
    return true;
}

EVP_PKEY* CertificateManager::unlockPersonalKey(const QString& id, const QString& password) const {
    clearLastError();
    if (!m_idx || !m_idx->personal.contains(id)) { setErr("无此个人证书"); return nullptr; }
    QString path = personalDir() + "/" + id + ".p12";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { setErr("无法打开 p12"); return nullptr; }
    QByteArray der = f.readAll();
    f.close();
    const uchar* p = reinterpret_cast<const uchar*>(der.constData());
    PKCS12* p12 = d2i_PKCS12(nullptr, &p, der.size());
    if (!p12) { setErr("d2i_PKCS12 失败: " + opensslErrStr()); return nullptr; }

    EVP_PKEY* pkey = nullptr;
    X509*      cert = nullptr;
    STACK_OF(X509)* ca = nullptr;
    if (!PKCS12_parse(p12, password.toUtf8().constData(), &pkey, &cert, &ca)) {
        PKCS12_free(p12);
        setErr("PKCS12_parse 失败: " + opensslErrStr());
        return nullptr;
    }
    PKCS12_free(p12);
    if (cert) X509_free(cert);
    if (ca)   sk_X509_pop_free(ca, X509_free);
    return pkey;
}

X509* CertificateManager::getPersonalCert(const QString& id) const {
    if (!m_idx || !m_idx->personal.contains(id)) return nullptr;
    QString path = personalDir() + "/" + id + ".p12";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return nullptr;
    QByteArray der = f.readAll();
    f.close();
    const uchar* p = reinterpret_cast<const uchar*>(der.constData());
    PKCS12* p12 = d2i_PKCS12(nullptr, &p, der.size());
    if (!p12) return nullptr;

    EVP_PKEY* pkey = nullptr;
    X509* cert = nullptr;
    STACK_OF(X509)* ca = nullptr;
    // 用空口令解析证书（不用私钥）—— OpenSSL 允许空口令解析出证书但 pkey 为 null
    if (!PKCS12_parse(p12, "", &pkey, &cert, &ca)) {
        PKCS12_free(p12);
        return nullptr;
    }
    PKCS12_free(p12);
    if (pkey) EVP_PKEY_free(pkey);
    if (ca)   sk_X509_pop_free(ca, X509_free);
    return cert;
}

bool CertificateManager::exportPersonal(const QString& id,
                                        const QString& destPath,
                                        const QString& originalPassword,
                                        const QString& newP12Password,
                                        QString* errorOut) {
    clearLastError();
    if (!m_idx || !m_idx->personal.contains(id)) {
        if (errorOut) *errorOut = QStringLiteral("个人证书不存在: %1").arg(id);
        return false;
    }
    if (newP12Password.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("新 PKCS#12 口令不能为空");
        return false;
    }

    // 1) 读取本地 .p12 并解锁私钥
    QString src = personalDir() + "/" + id + ".p12";
    QFile f(src);
    if (!f.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("无法打开本地证书: %1").arg(src);
        return false;
    }
    QByteArray der = f.readAll();
    f.close();
    const uchar* p = reinterpret_cast<const uchar*>(der.constData());
    PKCS12* p12 = d2i_PKCS12(nullptr, &p, der.size());
    if (!p12) {
        if (errorOut) *errorOut = QStringLiteral("d2i_PKCS12 失败: %1").arg(opensslErrStr());
        return false;
    }

    EVP_PKEY* pkey = nullptr;
    X509*      cert = nullptr;
    STACK_OF(X509)* ca = nullptr;
    if (!PKCS12_parse(p12, originalPassword.toUtf8().constData(), &pkey, &cert, &ca)) {
        PKCS12_free(p12);
        if (errorOut) *errorOut = QStringLiteral("PKCS12_parse 失败（口令错误或文件损坏）: %1").arg(opensslErrStr());
        return false;
    }
    PKCS12_free(p12);
    if (!cert || !pkey) {
        if (pkey) EVP_PKEY_free(pkey);
        if (cert) X509_free(cert);
        if (ca)   sk_X509_pop_free(ca, X509_free);
        if (errorOut) *errorOut = QStringLiteral("证书/私钥缺失");
        return false;
    }

    // 2) 用用户新口令重新打包 PKCS#12
    PKCS12* p12out = PKCS12_create(
        newP12Password.toUtf8().constData(),
        "bambooRat-export",
        pkey, cert, ca,
        NID_aes_256_cbc, NID_aes_256_cbc,
        PKCS12_DEFAULT_ITER, PKCS12_DEFAULT_ITER, 0);
    if (!p12out) {
        QString err = opensslErrStr();
        EVP_PKEY_free(pkey); X509_free(cert); if (ca) sk_X509_pop_free(ca, X509_free);
        if (errorOut) *errorOut = QStringLiteral("PKCS12_create 失败: %1").arg(err);
        return false;
    }

    QDir().mkpath(QFileInfo(destPath).absolutePath());
    BIO* bio = BIO_new_file(destPath.toUtf8().constData(), "wb");
    bool ok = (bio && i2d_PKCS12_bio(bio, p12out));
    if (bio) BIO_free(bio);
    PKCS12_free(p12out);
    EVP_PKEY_free(pkey);
    X509_free(cert);
    if (ca) sk_X509_pop_free(ca, X509_free);

    if (!ok) {
        if (errorOut) *errorOut = QStringLiteral("写 PKCS#12 失败: %1").arg(opensslErrStr());
        return false;
    }
    return true;
}

CertificateManager::CertInfo CertificateManager::importContact(const QString& pemPath) {
    clearLastError();
    QFile f(pemPath);
    if (!f.open(QIODevice::ReadOnly)) { setErr("无法打开: " + pemPath); return {}; }
    QByteArray pem = f.readAll();
    f.close();

    BIO* bio = BIO_new_mem_buf(pem.constData(), pem.size());
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert) { setErr("PEM_read_bio_X509 失败: " + opensslErrStr()); return {}; }

    CertInfo info;
    info.commonName      = extractNameByNid(cert, NID_commonName);
    info.email           = extractEmail(cert);
    info.sha1Fingerprint = sha1Hex(cert);
    info.id              = info.sha1Fingerprint;
    info.isPersonal      = false;
    info.sourcePath      = contactsDir() + "/" + info.sha1Fingerprint + ".pem";

    X509_NAME* issuerName = X509_get_issuer_name(cert);
    if (issuerName) {
        char buf[256] = {0};
        X509_NAME_get_text_by_NID(issuerName, NID_commonName, buf, sizeof(buf));
        info.issuer = QString::fromUtf8(buf);
    }
    ASN1_TIME* nb = X509_getm_notBefore(cert);
    ASN1_TIME* na = X509_getm_notAfter(cert);
    if (nb) info.notBefore = QDateTime::fromString(QString::fromUtf8(reinterpret_cast<char*>(nb->data)), Qt::ISODate);
    if (na) info.notAfter  = QDateTime::fromString(QString::fromUtf8(reinterpret_cast<char*>(na->data)), Qt::ISODate);
    X509_free(cert);

    // 把 PEM 写到 contacts/
    QDir().mkpath(contactsDir());
    QFile out(info.sourcePath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(pem);
        out.close();
    }

    if (!m_idx) m_idx = new IndexData;
    m_idx->contacts.insert(info.sha1Fingerprint, certInfoToJson(info));
    saveIndex();
    emit certAdded(info);
    return info;
}

CertificateManager::CertInfo CertificateManager::findByEmail(const QString& email) const {
    if (!m_idx || email.isEmpty()) return {};
    QString e = email.toLower();
    for (auto it = m_idx->contacts.begin(); it != m_idx->contacts.end(); ++it) {
        CertInfo c = certInfoFromJson(it.value().toObject());
        if (c.email.toLower() == e) return c;
    }
    return {};
}

QList<CertificateManager::CertInfo> CertificateManager::listContacts() const {
    QList<CertInfo> out;
    if (!m_idx) return out;
    for (auto it = m_idx->contacts.begin(); it != m_idx->contacts.end(); ++it) {
        out.append(certInfoFromJson(it.value().toObject()));
    }
    return out;
}

bool CertificateManager::removeContact(const QString& id) {
    if (!m_idx) return false;
    if (!m_idx->contacts.contains(id)) return false;
    QFile::remove(contactsDir() + "/" + id + ".pem");
    m_idx->contacts.remove(id);
    saveIndex();
    emit certRemoved(id);
    return true;
}

bool CertificateManager::exportContact(const QString& id, const QString& destPemPath, QString* errorOut) {
    clearLastError();
    if (!m_idx || !m_idx->contacts.contains(id)) {
        if (errorOut) *errorOut = QStringLiteral("联系人证书不存在: %1").arg(id);
        return false;
    }
    QString src = contactsDir() + "/" + id + ".pem";
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("无法打开源证书: %1").arg(src);
        return false;
    }
    QByteArray pem = in.readAll();
    in.close();
    if (pem.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("证书文件为空");
        return false;
    }
    QDir().mkpath(QFileInfo(destPemPath).absolutePath());
    QFile out(destPemPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut) *errorOut = QStringLiteral("无法写入目标: %1").arg(destPemPath);
        return false;
    }
    out.write(pem);
    out.close();
    return true;
}

QString CertificateManager::readContactPem(const QString& id) const {
    if (!m_idx || !m_idx->contacts.contains(id)) return {};
    QString src = contactsDir() + "/" + id + ".pem";
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) return {};
    QByteArray pem = in.readAll();
    in.close();
    return QString::fromUtf8(pem);
}