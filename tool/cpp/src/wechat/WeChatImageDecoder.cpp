#include "WeChatImageDecoder.h"

#include <openssl/evp.h>

namespace {

// AES-128-ECB 解密 data（长度会截断到 16 的倍数，无 padding）。
// 失败返回空 QByteArray。
QByteArray aesEcbDecrypt(const QByteArray& key16, const unsigned char* data, int len) {
    const int aligned = len - (len % 16);
    if (key16.size() != 16 || aligned < 16) return {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};
    QByteArray plain(aligned + 16, '\0');
    int outLen = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr,
                                 reinterpret_cast<const unsigned char*>(key16.constData()),
                                 nullptr) == 1;
    if (ok) {
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        ok = EVP_DecryptUpdate(ctx,
                               reinterpret_cast<unsigned char*>(plain.data()), &outLen,
                               data, aligned) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok || outLen <= 0) return {};
    plain.resize(outLen);
    return plain;
}

// 根据明文头部识别图片格式。
WeChatImageDecoder::Format detectFormat(const unsigned char* p, int len) {
    using Format = WeChatImageDecoder::Format;
    if (len < 12) return Format::Unknown;
    // JPEG: FF D8 FF
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return Format::Jpeg;
    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47 &&
        p[4] == 0x0D && p[5] == 0x0A && p[6] == 0x1A && p[7] == 0x0A) return Format::Png;
    // GIF: GIF87a / GIF89a
    if (p[0] == 0x47 && p[1] == 0x49 && p[2] == 0x46 && p[3] == 0x38 &&
        (p[4] == 0x37 || p[4] == 0x39) && p[5] == 0x61) return Format::Gif;
    // WebP: RIFF????WEBP
    if (p[0] == 0x52 && p[1] == 0x49 && p[2] == 0x46 && p[3] == 0x46 &&
        p[8] == 0x57 && p[9] == 0x45 && p[10] == 0x42 && p[11] == 0x50) return Format::WebP;
    // BMP: 42 4D
    if (p[0] == 0x42 && p[1] == 0x4D) return Format::Bmp;
    return Format::Unknown;
}

} // namespace

QByteArray WeChatImageDecoder::decryptV2(const QByteArray& dat, const QByteArray& key16,
                                         Format* fmtOut, QString* errOut) {
    if (fmtOut) *fmtOut = Format::Unknown;
    if (errOut) errOut->clear();

    if (key16.size() != 16) {
        if (errOut) *errOut = QStringLiteral("图片 key 长度不是 16 字节");
        return {};
    }
    // V2 magic: 0x07 0x08 'V' '2'，密文起点可能是 15 或 16
    if (dat.size() <= 47 ||
        (unsigned char)dat[0] != 0x07 || (unsigned char)dat[1] != 0x08 ||
        (unsigned char)dat[2] != 'V'  || (unsigned char)dat[3] != '2') {
        if (errOut) *errOut = QStringLiteral("不是 V2 .dat 文件（magic 不匹配或文件过小）");
        return {};
    }

    for (const int offset : {15, 16}) {
        QByteArray plain = aesEcbDecrypt(key16,
            reinterpret_cast<const unsigned char*>(dat.constData()) + offset,
            dat.size() - offset);
        if (plain.size() < 32) continue;
        const Format fmt = detectFormat(
            reinterpret_cast<const unsigned char*>(plain.constData()), plain.size());
        if (fmt == Format::Unknown) continue;
        // 次块不全 0 / 不全 FF（排除假阳性双块相同模式）
        const unsigned char* p = reinterpret_cast<const unsigned char*>(plain.constData());
        bool allZero = true, allFF = true;
        for (int i = 16; i < 32; ++i) {
            if (p[i] != 0x00) allZero = false;
            if (p[i] != 0xFF) allFF = false;
        }
        if (allZero || allFF) continue;
        if (fmtOut) *fmtOut = fmt;
        return plain;
    }

    if (errOut) *errOut = QStringLiteral("解密结果不是有效图片（key 可能不正确）");
    return {};
}

QByteArray WeChatImageDecoder::decryptEmoji(const QByteArray& raw, const QByteArray& key16) {
    if (key16.size() != 16 || raw.size() < 32) return {};
    QByteArray plain = aesEcbDecrypt(key16,
        reinterpret_cast<const unsigned char*>(raw.constData()), raw.size());
    if (plain.size() < 32) return {};
    if (detectFormat(reinterpret_cast<const unsigned char*>(plain.constData()),
                     plain.size()) == Format::Unknown) {
        return {};
    }
    return plain;
}

QString WeChatImageDecoder::formatExtension(Format fmt) {
    switch (fmt) {
    case Format::Jpeg: return QStringLiteral(".jpg");
    case Format::Png:  return QStringLiteral(".png");
    case Format::Gif:  return QStringLiteral(".gif");
    case Format::WebP: return QStringLiteral(".webp");
    case Format::Bmp:  return QStringLiteral(".bmp");
    default:           return QStringLiteral(".dat");
    }
}
