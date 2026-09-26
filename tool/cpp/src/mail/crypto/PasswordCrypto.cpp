#include "PasswordCrypto.h"
#include "CryptoEnvelopes.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <QCryptographicHash>
#include <QString>

// static 错误状态
namespace {
    QString s_lastError;

    void setErr(const QString& e) { s_lastError = e; }
}

QString PasswordCrypto::getLastError()   { return s_lastError; }
void    PasswordCrypto::clearLastError() { s_lastError.clear(); }

bool PasswordCrypto::isPasswordEnvelope(const QByteArray& data) {
    return data.size() >= 4 &&
           static_cast<uchar>(data[0]) == 'M' &&
           static_cast<uchar>(data[1]) == 'E' &&
           static_cast<uchar>(data[2]) == 'C' &&
           static_cast<uchar>(data[3]) == '1';
}

QByteArray PasswordCrypto::encrypt(const QString& password, const QByteArray& plaintext) {
    s_lastError.clear();
    if (password.isEmpty()) { setErr("口令为空"); return {}; }
    if (plaintext.isEmpty()) { setErr("明文为空"); return {}; }

    QByteArray pwdUtf8 = password.toUtf8();
    QByteArray salt(CryptoEnvelopes::SALT_LEN, '\0');
    QByteArray iv(CryptoEnvelopes::IV_LEN, '\0');
    if (RAND_bytes(reinterpret_cast<uchar*>(salt.data()), salt.size()) != 1 ||
        RAND_bytes(reinterpret_cast<uchar*>(iv.data()),   iv.size())   != 1) {
        setErr("RAND_bytes 失败");
        return {};
    }

    // PBKDF2-HMAC-SHA256 -> AES key
    QByteArray key(CryptoEnvelopes::PBKDF2_KEYLEN, '\0');
    if (PKCS5_PBKDF2_HMAC(pwdUtf8.constData(), pwdUtf8.size(),
                          reinterpret_cast<const uchar*>(salt.constData()), salt.size(),
                          CryptoEnvelopes::PBKDF2_ITERATIONS,
                          EVP_sha256(),
                          CryptoEnvelopes::PBKDF2_KEYLEN,
                          reinterpret_cast<uchar*>(key.data())) != 1) {
        setErr("PBKDF2 派生失败");
        return {};
    }

    // AES-256-GCM
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { setErr("EVP_CIPHER_CTX_new 失败"); return {}; }

    QByteArray tag(CryptoEnvelopes::TAG_LEN, '\0');
    int outLen1 = 0, outLen2 = 0;

    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
           && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CryptoEnvelopes::IV_LEN, nullptr) == 1
           && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                                 reinterpret_cast<const uchar*>(key.constData()),
                                 reinterpret_cast<const uchar*>(iv.constData())) == 1;

    QByteArray ciphertext(plaintext.size() + 16, '\0'); // 预留 block 空间
    if (ok) {
        ok = EVP_EncryptUpdate(ctx,
                               reinterpret_cast<uchar*>(ciphertext.data()), &outLen1,
                               reinterpret_cast<const uchar*>(plaintext.constData()),
                               plaintext.size()) == 1
          && EVP_EncryptFinal_ex(ctx,
                                 reinterpret_cast<uchar*>(ciphertext.data()) + outLen1, &outLen2) == 1;
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, CryptoEnvelopes::TAG_LEN,
                                 reinterpret_cast<uchar*>(tag.data())) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        setErr("AES-GCM 加密失败");
        return {};
    }
    ciphertext.resize(outLen1 + outLen2);

    // 拼装信封
    QByteArray envelope;
    envelope.reserve(4 + CryptoEnvelopes::SALT_LEN + CryptoEnvelopes::IV_LEN +
                     CryptoEnvelopes::TAG_LEN + ciphertext.size());
    envelope.append(CryptoEnvelopes::MAGIC, 4);
    envelope.append(salt);
    envelope.append(iv);
    envelope.append(tag);
    envelope.append(ciphertext);
    return envelope;
}

QByteArray PasswordCrypto::decrypt(const QString& password, const QByteArray& envelope) {
    s_lastError.clear();
    if (!isPasswordEnvelope(envelope)) {
        setErr("非 MEC1 信封");
        return {};
    }
    constexpr int HDR = 4 + CryptoEnvelopes::SALT_LEN + CryptoEnvelopes::IV_LEN + CryptoEnvelopes::TAG_LEN;
    if (envelope.size() < HDR) {
        setErr("信封长度不足");
        return {};
    }
    if (password.isEmpty()) {
        setErr("口令为空");
        return {};
    }

    const char* p = envelope.constData();
    QByteArray salt(p + 4, CryptoEnvelopes::SALT_LEN);
    QByteArray iv(p + 4 + CryptoEnvelopes::SALT_LEN, CryptoEnvelopes::IV_LEN);
    QByteArray tag(p + 4 + CryptoEnvelopes::SALT_LEN + CryptoEnvelopes::IV_LEN, CryptoEnvelopes::TAG_LEN);
    QByteArray ciphertext(p + HDR, envelope.size() - HDR);

    QByteArray pwdUtf8 = password.toUtf8();
    QByteArray key(CryptoEnvelopes::PBKDF2_KEYLEN, '\0');
    if (PKCS5_PBKDF2_HMAC(pwdUtf8.constData(), pwdUtf8.size(),
                          reinterpret_cast<const uchar*>(salt.constData()), salt.size(),
                          CryptoEnvelopes::PBKDF2_ITERATIONS,
                          EVP_sha256(),
                          CryptoEnvelopes::PBKDF2_KEYLEN,
                          reinterpret_cast<uchar*>(key.data())) != 1) {
        setErr("PBKDF2 派生失败");
        return {};
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) { setErr("EVP_CIPHER_CTX_new 失败"); return {}; }

    QByteArray plaintext(ciphertext.size() + 16, '\0');
    int outLen1 = 0, outLen2 = 0;

    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
           && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CryptoEnvelopes::IV_LEN, nullptr) == 1
           && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                                 reinterpret_cast<const uchar*>(key.constData()),
                                 reinterpret_cast<const uchar*>(iv.constData())) == 1
           && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, CryptoEnvelopes::TAG_LEN,
                                 reinterpret_cast<void*>(tag.data())) == 1
           && EVP_DecryptUpdate(ctx,
                                reinterpret_cast<uchar*>(plaintext.data()), &outLen1,
                                reinterpret_cast<const uchar*>(ciphertext.constData()),
                                ciphertext.size()) == 1;

    if (ok) {
        // GCM FinalEx 会校验 tag；tag 不匹配则返回 0
        ok = EVP_DecryptFinal_ex(ctx,
                                 reinterpret_cast<uchar*>(plaintext.data()) + outLen1, &outLen2) == 1;
        if (!ok) setErr("AES-GCM tag 校验失败（口令错误或数据被篡改）");
    }
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) return {};
    plaintext.resize(outLen1 + outLen2);
    return plaintext;
}

QString PasswordCrypto::fingerprint(const QByteArray& envelope) {
    if (!isPasswordEnvelope(envelope)) return {};
    QByteArray hash = QCryptographicHash::hash(envelope, QCryptographicHash::Sha256);
    return QByteArray(hash.left(8)).toHex().toUpper();
}