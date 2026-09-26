#include "core/Crypto.h"
#include <QByteArray>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif

Crypto& Crypto::instance() {
    static Crypto c;
    return c;
}

Crypto::Crypto(QObject* parent) : QObject(parent) {}

bool Crypto::isAvailable() const {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QString Crypto::encrypt(const QString& plaintext) {
    if (plaintext.isEmpty()) return {};
#ifdef Q_OS_WIN
    QByteArray data = plaintext.toUtf8();
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(data.data());
    input.cbData = static_cast<DWORD>(data.size());

    DATA_BLOB output;
    if (CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        QByteArray encrypted(reinterpret_cast<const char*>(output.pbData),
                              static_cast<int>(output.cbData));
        LocalFree(output.pbData);
        return QString::fromUtf8(encrypted.toBase64());
    }
    return {};
#else
    // Fallback: no encryption on non-Windows
    return plaintext;
#endif
}

QString Crypto::decrypt(const QString& ciphertextBase64) {
    if (ciphertextBase64.isEmpty()) return {};
#ifdef Q_OS_WIN
    QByteArray encrypted = QByteArray::fromBase64(ciphertextBase64.toUtf8());
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(encrypted.data());
    input.cbData = static_cast<DWORD>(encrypted.size());

    DATA_BLOB output;
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        QByteArray decrypted(reinterpret_cast<const char*>(output.pbData),
                             static_cast<int>(output.cbData));
        LocalFree(output.pbData);
        return QString::fromUtf8(decrypted);
    }
    return {};
#else
    return ciphertextBase64;
#endif
}
