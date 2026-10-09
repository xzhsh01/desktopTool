#pragma once

#include <QByteArray>
#include <QString>

// WeChat 4.x 图片 .dat 解密器。
//
// 格式（见 WeChatKeyExtractor.cpp verifyKeyByFullOracle / tryOffsetVerify）：
//   V2 .dat：byte[0..3] = 0x07 0x08 'V' '2'，头部 15 或 16 字节，
//            之后是 AES-128-ECB 密文（无 padding，尾部 1~15 字节截断丢弃）。
//   emoji ：整文件即 AES-128-ECB 密文（offset 0，16 字节对齐，无 padding）。
class WeChatImageDecoder {
public:
    enum class Format {
        Unknown,
        Jpeg,
        Png,
        Gif,
        WebP,
        Bmp,
    };

    // 解密 V2 .dat。key16 为 16 字节 AES-128-ECB key。
    // 自动尝试密文起点 offset 15 / 16。
    // fmtOut / errOut 可为 nullptr。失败返回空 QByteArray。
    static QByteArray decryptV2(const QByteArray& dat, const QByteArray& key16,
                                Format* fmtOut = nullptr, QString* errOut = nullptr);

    // 解密 4.x emoji 缓存文件（整文件 ECB，offset 0）。
    // key 错误或结果不是图片时返回空 QByteArray（调用方自行兜底原字节）。
    static QByteArray decryptEmoji(const QByteArray& raw, const QByteArray& key16);

    static QString formatExtension(Format fmt);
};
