#include "mail/ImapClient.h"
#include "mail/MailStore.h"
#include "core/Logger.h"

#include <QSslSocket>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QStringConverter>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

// IMAP 命令标签：递增 A001, A002 ...
struct Tag {
    int n = 0;
    QString next() { return QString("A%1").arg(++n, 3, 10, QChar('0')); }
};

bool readLine(QSslSocket& sock, int timeoutMs, QString* out) {
    if (!sock.waitForReadyRead(timeoutMs)) return false;
    *out = QString::fromUtf8(sock.readLine()).trimmed();
    return !out->isEmpty();
}

// 读取多行响应：直到 "<tag> OK/NO/BAD ..." 终止行
// 重要：必须正确处理 IMAP literal（" {N}\r\n<N字节数据>"）。
// literal 数据块以字节数为准，不保证包含 \n；若用 canReadLine() 等换行，
// 数据流可能一直没有 \n → 内层 while 不进 → 外层 waitForReadyRead 阻塞 15s 超时。
// rawStream（可选）：按序保留服务器发来的所有原始字节（行 + literal），
//   供 ENVELOPE 解析器按字节扫描（literal 字节数语义在 QString 中会失真）。
// onLiteralProgress（可选）：literal 接收过程中回调（bytesDone, bytesExpected），
//   用于 fetchBody 等大响应向 UI 投递拉取进度。
bool readResponse(QSslSocket& sock, const QString& expectTag, int timeoutMs,
                  QStringList* lines, QString* statusLine,
                  QByteArray* rawStream = nullptr,
                  std::function<void(qint64, qint64)> onLiteralProgress = nullptr) {
    QString line;
    int literalReads = 0;
    while (true) {
        if (!sock.waitForReadyRead(timeoutMs)) {
            if (statusLine) *statusLine = QString("waitForReadyRead 超时 %1s").arg(timeoutMs / 1000);
            return false;
        }
        while (sock.canReadLine()) {
            QByteArray rawLine = sock.readLine();
            if (rawStream) rawStream->append(rawLine);
            line = QString::fromUtf8(rawLine).trimmed();
            // 检测行尾的 literal 标记：必须是 " {N}\r\n" 形式（N 是非负整数）
            // 139 邮箱的 FETCH 响应：* N FETCH (...) BODY[HEADER.FIELDS ...] {534}
            if (line.endsWith('}')) {
                int braceOpen = line.lastIndexOf('{');
                if (braceOpen >= 0) {
                    QString numStr = line.mid(braceOpen + 1, line.size() - braceOpen - 2);
                    bool ok = false;
                    int literalSize = numStr.toInt(&ok);
                    // 接受最大 200MB literal：
                    //   - 10MB 上限太苛刻，正规业务附件（截图、PDF、压缩包）经常超过；
                    //   - 服务器声明后由 literalSize 限制真正的 read 次数，天然防 OOM；
                    //   - 超过此阈值时跳过 literal 读取并打 WARN，便于排查。
                    constexpr int kMaxLiteralBytes = 200 * 1024 * 1024;
                    if (ok && literalSize >= 0 && literalSize < kMaxLiteralBytes) {
                        // 把 literal 之前的部分（FETCH 元数据）也保留
                        lines->append(line);
                        // 读 literal 数据（按字节数，不依赖 \n）
                        QByteArray literal;
                        literal.reserve(literalSize);
                        QElapsedTimer litTimer; litTimer.start();
                        while (literal.size() < literalSize) {
                            if (!sock.waitForReadyRead(timeoutMs)) {
                                if (statusLine) *statusLine = QString(
                                    "literal 读取超时: 已读 %1/%2 字节, 耗时 %3ms")
                                    .arg(literal.size()).arg(literalSize).arg(litTimer.elapsed());
                                if (!literal.isEmpty()) lines->append(QString::fromUtf8(literal));
                                return false;
                            }
                            QByteArray chunk = sock.read(literalSize - literal.size());
                            if (chunk.isEmpty()) break;
                            literal.append(chunk);
                            // literal 接收进度回调（按 chunk 触发；调用方负责节流）
                            if (onLiteralProgress)
                                onLiteralProgress(literal.size(), literalSize);
                        }
                        if (rawStream) rawStream->append(literal);
                        lines->append(QString::fromUtf8(literal));
                        ++literalReads;
                        // 服务器在 literal 之后会补 CRLF，继续读下一行
                        continue;
                    }
                }
            }
            if (line.startsWith(expectTag + " ")) {
                if (statusLine) *statusLine = line;
                return true;
            }
            lines->append(line);
        }
    }
}

bool sendCmd(QSslSocket& sock, Tag& tag, const QString& cmd, int timeoutMs,
             QStringList* resp, QString* status, QString* err,
             QByteArray* rawStream = nullptr,
             std::function<void(qint64, qint64)> onLiteralProgress = nullptr) {
    QString t = tag.next();
    QElapsedTimer cmdTimer; cmdTimer.start();
    sock.write((t + " " + cmd + "\r\n").toUtf8());
    sock.waitForBytesWritten(5000);
    resp->clear();
    if (status) status->clear();
    bool ok = readResponse(sock, t, timeoutMs, resp, status, rawStream, onLiteralProgress);
    qint64 elapsedMs = cmdTimer.elapsed();
    if (elapsedMs >= 5000) {
        // 仅记录耗时 ≥5s 的命令,便于排查慢服务器
        Logger::instance().info(
            QString("IMAP 命令耗时 %1ms / 超时 %2ms, ok=%3, tag=%4, cmd 前 60 字=%5, resp 行数=%6")
                .arg(elapsedMs).arg(timeoutMs).arg(ok ? "Y" : "N").arg(t)
                .arg(cmd.left(60)).arg(resp->size()),
            "mail");
    }
    if (!ok) {
        if (status) *status = QString("IMAP 命令超时 (%1s, 实际 %2s): %3")
                                  .arg(timeoutMs / 1000)
                                  .arg(elapsedMs / 1000)
                                  .arg(cmd);
        if (err) *err = "IMAP 命令超时: " + cmd;
        return false;
    }
    // OK = 成功；NO/BAD = 失败
    return status && status->contains(" OK ");
}

// 按声明的 charset 把原始字节解码为 QString（GBK/GB2312/GB18030/UTF-8 等）
// 注意：Qt6 QStringConverter 只支持 UTF/Latin1/System，不支持 GBK 系，
// Windows 下用 MultiByteToWideChar(CP936) 转码。
#ifdef Q_OS_WIN
static QString gbkToUnicode(const QByteArray& bytes) {
    if (bytes.isEmpty()) return {};
    int wlen = MultiByteToWideChar(936, 0, bytes.constData(),
                                   static_cast<int>(bytes.size()), nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(936, 0, bytes.constData(),
                        static_cast<int>(bytes.size()), &w[0], wlen);
    return QString::fromStdWString(w);
}
#endif

QString decodeBytes(const QByteArray& bytes, const QString& charset) {
    if (charset.compare("utf-8", Qt::CaseInsensitive) == 0 ||
        charset.compare("utf8", Qt::CaseInsensitive) == 0) {
        return QString::fromUtf8(bytes);
    }
    if (charset.isEmpty()) {
        // charset 未声明：UTF-8 校验（无 U+FFFD）通过则用 UTF-8，
        // 否则按 GBK(CP936) 解码 —— 国内邮箱（139/QQ/163）大量 GBK 正文
        QString u8 = QString::fromUtf8(bytes);
        if (!u8.contains(QChar(0xFFFD))) return u8;
#ifdef Q_OS_WIN
        QString g = gbkToUnicode(bytes);
        if (!g.isEmpty()) return g;
#endif
        return u8;
    }
    QString c = charset.toLower();
    if (c == "gbk" || c == "gb2312" || c == "gb18030" || c.startsWith("gb")) {
#ifdef Q_OS_WIN
        QString r = gbkToUnicode(bytes);
        if (!r.isEmpty()) return r;
#endif
        return QString::fromUtf8(bytes);   // 非 Windows 兜底
    }
    auto e = QStringConverter::encodingForName(charset.toUtf8());
    if (e.has_value()) {
        QStringDecoder decoder(e.value(),
                               QStringConverter::Flag::Stateless |
                               QStringConverter::Flag::ConvertInvalidToNull);
        QString r = decoder(bytes);
        if (!r.isEmpty()) return r;
    }
    return QString::fromUtf8(bytes);   // 兜底按 UTF-8
}

QString decodeMime(const QString& s) {
    // =?UTF-8?B?xxxx?=  → 按 charset 解码
    QString out = s;
    static const QRegularExpression re("=\\?([^?]+)\\?([BbQq])\\?([^?]*)\\?=");
    QRegularExpressionMatchIterator it = re.globalMatch(out);
    if (!it.hasNext()) return out;
    QStringList parts;
    int lastEnd = 0;
    while (it.hasNext()) {
        auto m = it.next();
        parts.append(out.mid(lastEnd, m.capturedStart() - lastEnd));
        QString charset = m.captured(1);
        QString enc     = m.captured(2);
        QString data    = m.captured(3);
        if (enc.compare("B", Qt::CaseInsensitive) == 0) {
            // base64：解出原始字节后必须按 charset（如 GBK）解码，
            // 直接 fromUtf8 会把 GBK 字节变成 U+FFFD 乱码
            parts.append(decodeBytes(QByteArray::fromBase64(data.toUtf8()), charset));
        } else {
            // Q encoding: _ → ' '，=XX 十六进制转义还原为原始字节
            data.replace('_', ' ');
            QByteArray bytes;
            for (int i = 0; i < data.size(); ++i) {
                if (data[i] == '=' && i + 2 < data.size()) {
                    bool ok = false;
                    int b = data.mid(i + 1, 2).toInt(&ok, 16);
                    if (ok) { bytes.append(char(b)); i += 2; continue; }
                }
                bytes.append(data[i].toLatin1());
            }
            parts.append(decodeBytes(bytes, charset));
        }
        lastEnd = m.capturedEnd();
    }
    parts.append(out.mid(lastEnd));
    return parts.join("");
}

// 解析邮件头中的字段（单值）
QString extractHeader(const QString& headers, const QString& field) {
    static const QRegularExpression re(
        QString("^%1:[ \\t]*(.+(?:\\r?\\n[ \\t].+)*)").arg(field),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    auto m = re.match(headers);
    if (!m.hasMatch()) return {};
    QString v = m.captured(1).trimmed();
    // 折叠换行（去除行首空白）
    v.replace(QRegularExpression("\\r?\\n[ \\t]+"), " ");
    return decodeMime(v);
}

// 解析逗号/分号分隔的地址列表（保留 email）
QStringList extractAddresses(const QString& headers, const QString& field) {
    QStringList out;
    QString line = extractHeader(headers, field);
    if (line.isEmpty()) return out;
    // 简化：每段 "<email>"，或裸 email
    QRegularExpression re("<([^>]+)>");
    int last = 0;
    auto it = re.globalMatch(line);
    while (it.hasNext()) {
        auto m = it.next();
        out.append(m.captured(1).trimmed());
        last = m.capturedEnd();
    }
    if (out.isEmpty()) {
        // 无 <> 的情况：把整段按 ,; 分割
        for (const QString& p : line.split(QRegularExpression("[,;]"), Qt::SkipEmptyParts)) {
            out.append(p.trimmed());
        }
    }
    return out;
}

// ── 传输编码解码（quoted-printable / base64）──────────────────────────
// 字节级 quoted-printable 解码：软换行(=\r\n / =\n) + =XX 十六进制
static QByteArray qpDecodeBytes(const QByteArray& in) {
    QByteArray out;
    out.reserve(in.size());
    auto hexv = [](uchar x) -> int {
        if (x >= '0' && x <= '9') return x - '0';
        if (x >= 'A' && x <= 'F') return x - 'A' + 10;
        if (x >= 'a' && x <= 'f') return x - 'a' + 10;
        return -1;
    };
    for (int i = 0; i < in.size(); ++i) {
        uchar c = static_cast<uchar>(in[i]);
        if (c == '=') {
            if (i + 2 < in.size() && in[i+1] == '\r' && in[i+2] == '\n') { i += 2; continue; }
            if (i + 1 < in.size() && in[i+1] == '\n') { i += 1; continue; }
            if (i + 2 < in.size()) {
                int hi = hexv(static_cast<uchar>(in[i+1]));
                int lo = hexv(static_cast<uchar>(in[i+2]));
                if (hi >= 0 && lo >= 0) {
                    out.append(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
        }
        out.append(static_cast<char>(c));
    }
    return out;
}

// 未知 charset 的正文字节 → QString：UTF-8 校验（无 U+FFFD 替换符）通过则用 UTF-8，
// 否则按 GBK(CP936) 解码 —— 国内邮箱（139/QQ/163）大量 GBK 正文
static QString decodeBodyBytes(const QByteArray& bytes) {
    QString u8 = QString::fromUtf8(bytes);
    if (!u8.contains(QChar(0xFFFD))) return u8;
#ifdef Q_OS_WIN
    QString g = gbkToUnicode(bytes);
    if (!g.isEmpty()) return g;
#endif
    return u8;
}

static QString decodeTransferEncoding(const QString& enc, const QString& raw, const QString& charset = QString()) {
    QString e = enc.trimmed().toLower();
    if (e.isEmpty() || e == "7bit" || e == "8bit" || e == "binary") {
        // 8bit 未编码正文：Latin-1 保字节还原原始字节后按声明 charset 解码
        // （直接 return raw 会把 0x80-0xFF 字节当 Latin-1 字符 → 乱码）
        return decodeBytes(raw.toLatin1(), charset);
    }
    QString text = raw;
    // 去除软换行（QP 行末的 =）
    if (e == "quoted-printable") {
        text.replace(QRegularExpression("=\\r?\\n"), "");
        // text 来自 QString::fromLatin1(lit)，每个 QChar 的 unicode 码点 = 原始字节 (0..255)。
        // 必须按字节还原（toLatin1）再做 QP 解码；若 toUtf8 会把 0x80-0xFF 字节重编码为
        // 2 字节 UTF-8，导致 GBK/Big5 等非 UTF-8 编码邮件的 QP 解码整体错位。
        QByteArray in = text.toLatin1();
        QByteArray out;
        out.reserve(in.size());
        for (int i = 0; i < in.size(); ++i) {
            uchar c = static_cast<uchar>(in[i]);
            if (c == '=' && i + 2 < in.size()) {
                auto hexv = [](uchar x) -> int {
                    if (x >= '0' && x <= '9') return x - '0';
                    if (x >= 'A' && x <= 'F') return x - 'A' + 10;
                    if (x >= 'a' && x <= 'f') return x - 'a' + 10;
                    return -1;
                };
                int hi = hexv(static_cast<uchar>(in[i+1]));
                int lo = hexv(static_cast<uchar>(in[i+2]));
                if (hi >= 0 && lo >= 0) {
                    out.append(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            out.append(static_cast<char>(c));
        }
        // 按声明 charset 解码（GBK 系走 CP936，避免 fromUtf8 产生 U+FFFD 乱码）
        return decodeBytes(out, charset);
    }
    if (e == "base64") {
        QByteArray in = raw.toLatin1();
        // 去掉所有空白字符(base64 是 A-Za-z0-9+/=,不含空白)
        for (int i = in.size() - 1; i >= 0; --i) {
            char c = in[i];
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') in.remove(i, 1);
        }
        QByteArray decoded = QByteArray::fromBase64(in);
        // 按声明 charset 解码（同上，GBK 系走 CP936）
        return decodeBytes(decoded, charset);
    }
    return text;
}

// RFC2047 解码 filename/header value：= ?charset?B/Q?data?= → QString
static QString decodeMimeWord(const QString& s) {
    if (s.isEmpty() || !s.startsWith("=?") || !s.contains("?=")) return s;
    QRegularExpression re("=\\?([^?]+)\\?([BbQq])\\?([^?]*)\\?=");
    auto m = re.match(s);
    if (!m.hasMatch()) return s;
    QByteArray raw;
    if (m.captured(2).toUpper() == "B") {
        raw = QByteArray::fromBase64(m.captured(3).toLatin1());
    } else {
        // QP-encoded: "_"→空格; "=XX" → byte; 软换行 = 已剥
        const QByteArray in = m.captured(3).toLatin1();
        for (int i = 0; i < in.size(); ++i) {
            char c = in[i];
            if (c == '_') raw.append(' ');
            else if (c == '=' && i + 2 < in.size()) {
                auto hexv = [](uchar x) -> int {
                    if (x >= '0' && x <= '9') return x - '0';
                    if (x >= 'A' && x <= 'F') return x - 'A' + 10;
                    if (x >= 'a' && x <= 'f') return x - 'a' + 10;
                    return -1;
                };
                int hi = hexv(static_cast<uchar>(in[i+1]));
                int lo = hexv(static_cast<uchar>(in[i+2]));
                if (hi >= 0 && lo >= 0) { raw.append(static_cast<char>((hi << 4) | lo)); i += 2; }
                else raw.append(c);
            } else raw.append(c);
        }
    }
    // Qt6 无 QTextCodec；复用 decodeBodyBytes：UTF-8 优先，无效字节按 GBK 回退
    // （国内邮箱（139/QQ/163）大量 GBK 编码的 RFC2047 encoded-word）
    return decodeBodyBytes(raw);
}

// RFC5987 解码 filename* 的 value 部分：UTF-8''xxxx 或 UTF-8'lang'xxxx
// 返回 percent-encoded 字符串解码后的 QString
static QString decodeRfc5987(const QString& charset, const QString& lang, const QString& value) {
    Q_UNUSED(lang);
    QByteArray bytes;
    bytes.reserve(value.size());
    for (int i = 0; i < value.size(); ++i) {
        QChar c = value[i];
        if (c == '%' && i + 2 < value.size()) {
            auto hexv = [](QChar x) -> int {
                ushort u = x.unicode();
                if (u >= '0' && u <= '9') return u - '0';
                if (u >= 'A' && u <= 'F') return u - 'A' + 10;
                if (u >= 'a' && u <= 'f') return u - 'a' + 10;
                return -1;
            };
            int hi = hexv(value[i+1]);
            int lo = hexv(value[i+2]);
            if (hi >= 0 && lo >= 0) {
                bytes.append(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        // 显式 latin-1 转 char 即可（RFC5987 字符都是 ASCII）
        if (c.unicode() < 256) bytes.append(static_cast<char>(c.unicode()));
    }
    QString cs = charset.trimmed().toLower();
    if (cs == "utf-8" || cs == "utf8" || cs == "us-ascii" || cs == "ascii") {
        return QString::fromUtf8(bytes);
    }
    // 其它 charset：UTF-8 优先，无效字节 GBK 回退
    return decodeBodyBytes(bytes);
}

// 从 MIME header 文本里提取 filename / filename*，按 RFC 优先级返回解码后的值：
//   1) filename*=charset'lang'value   (RFC5987 / RFC6266)
//   2) filename="..."                  (RFC2047)
//   3) Content-Type name= 兜底
static QString extractDecodedFilename(const QString& headers, const QString& ctype) {
    // 1) filename*= (RFC5987 / RFC6266)
    static const QRegularExpression fStarRe(
        "filename\\*\\s*=\\s*([^;\\r\\n]+)",
        QRegularExpression::CaseInsensitiveOption);
    auto m = fStarRe.match(headers);
    if (m.hasMatch()) {
        QString raw = m.captured(1).trimmed();
        if (raw.startsWith('"')) raw = raw.mid(1);
        if (raw.endsWith('"'))   raw.chop(1);
        // RFC 5987 格式: charset'[lang]'value, lang 可空（utf-8''xxxx）
        // 第 1 个 ' 是 charset 后；第 2 个 ' 是 lang 后（紧贴第 1 个表示 lang 空）；
        // value 从第 2 个 ' 之后开始。
        int q1 = raw.indexOf('\'');
        int q2 = (q1 >= 0) ? raw.indexOf('\'', q1 + 1) : -1;
        if (q1 > 0 && q2 >= q1 + 1) {
            // q2 == q1+1 表示 lang 为空（两个 ' 紧挨），也合法
            return decodeRfc5987(raw.left(q1),
                                  raw.mid(q1 + 1, q2 - q1 - 1),
                                  raw.mid(q2 + 1));
        }
        // 没引号分隔的回退按 RFC2047 解
        return decodeMimeWord(raw);
    }
    // 2) filename= (RFC2047 解码)
    static const QRegularExpression fRe(
        "filename\\s*=\\s*\"?([^;\\r\\n\"]+)\"?",
        QRegularExpression::CaseInsensitiveOption);
    auto m2 = fRe.match(headers);
    if (m2.hasMatch()) return decodeMimeWord(m2.captured(1).trimmed());
    // 3) Content-Type name=
    static const QRegularExpression nRe(
        "\\bname\\s*=\\s*\"?([^;\\r\\n\"]+)\"?",
        QRegularExpression::CaseInsensitiveOption);
    auto m3 = nRe.match(ctype);
    if (m3.hasMatch()) return decodeMimeWord(m3.captured(1).trimmed());
    return {};
}

// 从 multipart part 的 header 段中读出 charset / Content-Type / Content-Transfer-Encoding
static void readPartHeaders(const QString& ph,
                            QString* ctype, QString* cte, QString* charset) {
    static const QRegularExpression ctypeRe(
        "^Content-Type:[ \\t]*(.+)(?:\\r?\\n[ \\t].+)*",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    auto cm = ctypeRe.match(ph);
    if (cm.hasMatch()) {
        *ctype = cm.captured(1).trimmed().toLower();
        QRegularExpression csRe("charset=\"?([^\"\\s;]+)\"?",
                                QRegularExpression::CaseInsensitiveOption);
        auto csm = csRe.match(*ctype);
        if (csm.hasMatch()) *charset = csm.captured(1);
    }
    static const QRegularExpression cteRe(
        "^Content-Transfer-Encoding:[ \\t]*(.+)",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    auto em = cteRe.match(ph);
    if (em.hasMatch()) *cte = em.captured(1).trimmed();
}

// 简易 HTML → 纯文本（text/html 回退时用）：
// 去 script/style 块、块级标签转换行、剥其余标签、解码常用实体
static QString htmlToPlainText(const QString& html) {
    QString t = html;
    t.remove(QRegularExpression(
        "<(script|style)[^>]*>.*?</\\1>",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption));
    t.replace(QRegularExpression(
        "</?(p|div|br|tr|li|h[1-6]|table|blockquote|pre)[^>]*>",
        QRegularExpression::CaseInsensitiveOption), "\n");
    t.remove(QRegularExpression("<[^>]+>"));
    t.replace("&nbsp;", " ", Qt::CaseInsensitive);
    t.replace("&lt;", "<", Qt::CaseInsensitive);
    t.replace("&gt;", ">", Qt::CaseInsensitive);
    t.replace("&quot;", "\"", Qt::CaseInsensitive);
    t.replace("&amp;", "&", Qt::CaseInsensitive);
    t.replace(QRegularExpression("\\n{3,}"), "\n\n");
    return t.trimmed();
}

// 从 RFC822 邮件中抽取正文（顶层入口；下方定义）
void extractBodyParts(const QString& raw, QString* plainOut, QString* htmlOut);

// 从 RFC822 邮件中抽取正文（递归处理嵌套 multipart）：
// plainOut 出参 = text/plain，htmlOut 出参 = text/html（原样保留，供富文本渲染）
static void collectBodyParts(const QString& headers, const QString& body,
                             QString* plainOut, QString* htmlOut) {
    QString ctype, cte, charset;
    readPartHeaders(headers, &ctype, &cte, &charset);
    if (ctype.contains("multipart/")) {
        // 部分 MTA 把 boundary 属性写成大写 "BOUNDARY=..."（如 HMail Webmail），必须大小写不敏感
        QRegularExpression re("boundary=\"?([^\"\\s;]+)\"?",
                              QRegularExpression::CaseInsensitiveOption);
        auto m = re.match(headers);
        if (!m.hasMatch()) {
            Logger::instance().warn(
                QString("collectBodyParts: multipart 但无 boundary: ctype=\"%1\"").arg(ctype), "mail");
            return;
        }
        const QString bnd = m.captured(1);
        QString b2 = body;
        // boundary 分隔符：\r?\n--<bnd> 后跟换行（起始/中间边界）或 --（终止边界 --bnd--）。
        // 主动消费 boundary 行末换行/终止 --，避免每个 part 前缀残留 \r\n 干扰 header 解析，
        // 避免终止边界尾巴 "--\r\n" 混入正文。兼容 \r\n 与 \n（部分 MTA 只用 \n 分隔）。
        QRegularExpression splitRe(QStringLiteral("\\r?\\n--") +
                                   QRegularExpression::escape(bnd) +
                                   QStringLiteral("(?:\\r?\\n|--)"));
        // 首 part 前可能没有行尾分隔符（body 以 "--bnd" 或 "\n--bnd" 开头）→ 补前缀保证 split 命中
        if (b2.startsWith(QStringLiteral("--") + bnd)) {
            b2.prepend(QStringLiteral("\r\n"));
        } else if (b2.startsWith(QStringLiteral("\n--") + bnd)) {
            b2.prepend(QChar('\r'));
        }
        const QStringList parts = b2.split(splitRe);
        Logger::instance().info(
            QString("collectBodyParts: multipart ctype=\"%1\" boundary=\"%2\" parts=%3 bodyLen=%4")
                .arg(ctype, bnd).arg(parts.size()).arg(b2.size()),
            "mail");
        for (const QString& p : parts) {
            // 跳过 preamble / 终止边界后的 epilogue（"-\r\n"、纯空白尾巴等空段）
            if (p.trimmed().isEmpty()) continue;
            int ps = p.indexOf(QStringLiteral("\r\n\r\n"));
            if (ps < 0) ps = p.indexOf(QStringLiteral("\n\n"));
            if (ps < 0) continue;
            const QString partHeaders = p.left(ps);
            // HMail Webmail / 某些中文邮件系统把 Content-Type 写在最顶层第一行，
            // split 后第一个 part 残留的是 From/To/Subject 等顶层 headers，不含 Content-Type。
            // 没有 Content-Type 头的 part 不是有效 MIME part，直接跳过。
            if (!partHeaders.contains(
                    QRegularExpression(QStringLiteral("^Content-Type:"),
                                       QRegularExpression::CaseInsensitiveOption |
                                       QRegularExpression::MultilineOption))) {
                Logger::instance().info(
                    "collectBodyParts: 跳过无 Content-Type 的 part（HMail 残留顶层 headers）", "mail");
                continue;
            }
            collectBodyParts(partHeaders,
                             p.mid(ps + (p.at(ps) == QLatin1Char('\r') ? 4 : 2)),
                             plainOut, htmlOut);
        }
        return;
    }
    // message/rfc822 转发包：body 是另一封完整 RFC822 邮件，原样再走一遍 extractBodyParts
    if (ctype.startsWith("message/rfc822")) {
        Logger::instance().info("collectBodyParts: message/rfc822 forwarded, recursing", "mail");
        QString innerPlain, innerHtml;
        extractBodyParts(body, &innerPlain, &innerHtml);
        if (plainOut && plainOut->isEmpty() && !innerPlain.isEmpty()) *plainOut = innerPlain;
        if (htmlOut  && htmlOut->isEmpty()  && !innerHtml.isEmpty())  *htmlOut  = innerHtml;
        return;
    }
    QString enc = cte.isEmpty() ? extractHeader(headers, "Content-Transfer-Encoding").toLower()
                                : cte;
    QString decoded = decodeTransferEncoding(enc, body, charset);
    if (ctype.contains("text/plain")) {
        if (plainOut && plainOut->isEmpty()) *plainOut = decoded;
        Logger::instance().info(
            QString("collectBodyParts: text/plain cte=\"%1\" charset=\"%2\" decoded=%3")
                .arg(enc, charset).arg(decoded.size()),
            "mail");
    } else if (ctype.contains("text/html")) {
        if (htmlOut && htmlOut->isEmpty()) *htmlOut = decoded;
        Logger::instance().info(
            QString("collectBodyParts: text/html cte=\"%1\" charset=\"%2\" decoded=%3")
                .arg(enc, charset).arg(decoded.size()),
            "mail");
    } else {
        Logger::instance().info(
            QString("collectBodyParts: skip non-text ctype=\"%1\" cte=\"%2\" bodyLen=%3")
                .arg(ctype, enc).arg(body.size()),
            "mail");
    }
}

// 从 RFC822 邮件中递归收集附件元数据：
//   - 判定为附件：Content-Disposition: attachment 强信号；
//     否则启发式（非 text/* / multipart/* / message/* 也视为附件，如 image/png）
//   - section 编号：multipart 内 part 序号递增（嵌套用 "." 连接）；
//     顶层非 multipart 的 section = "1"（单段邮件一般无附件）
//   - filename 优先取 Content-Disposition filename，其次 Content-Type name=，再回退到子类型
//   - 大小按 encoding 估算（base64 → ×3/4；QP ≈ 1:1；其他按 body 字节数）
static void collectAttachments(const QString& headers, const QString& body,
                               const QString& section, int* nextIdx,
                               QList<MailStore::Attachment>* atts) {
    QString ctype, cte, charset;
    readPartHeaders(headers, &ctype, &cte, &charset);
    if (ctype.contains("multipart/")) {
        // 部分 MTA 把 boundary 属性写成大写 "BOUNDARY=..."（如 HMail Webmail），必须大小写不敏感
        QRegularExpression re("boundary=\"?([^\"\\s;]+)\"?",
                              QRegularExpression::CaseInsensitiveOption);
        auto m = re.match(headers);
        if (!m.hasMatch()) return;
        const QString bnd = m.captured(1);
        QString b2 = body;
        // 兼容 \r\n 与 \n 两种行尾（部分 MTA/客户端只用 \n 分隔 boundary）
        QRegularExpression splitRe(QStringLiteral("\\r?\\n--") + QRegularExpression::escape(bnd));
        if (b2.startsWith(QStringLiteral("--") + bnd))
            b2.prepend(QStringLiteral("\r\n"));
        else if (b2.startsWith(QStringLiteral("\n--") + bnd))
            b2.prepend(QChar('\r'));
        // IMAP 的 BODY[] section 编号是"相对父容器的子 part 序号"，每个
        // multipart 容器内部的子 part 都从 1 重新开始（嵌套用 "." 连接）。
        // 旧实现用跨层级的全局计数器 nextIdx 递增，对嵌套 multipart/releated
        // 内嵌图片的邮件会错号（如 related 内第一段拿到 1.2 而非 1.1），导致
        // FETCH 拉到别的 part / 整段 MIME 源码 → 下载内容乱码。故改用局部计数。
        int sub = 0;
        for (const QString& p : b2.split(splitRe)) {
            if (p.startsWith(QStringLiteral("--"))) continue;
            int ps = p.indexOf(QStringLiteral("\r\n\r\n"));
            if (ps < 0) ps = p.indexOf(QStringLiteral("\n\n"));
            if (ps < 0) continue;
            ++sub;
            const QString childSection = section.isEmpty()
                ? QString::number(sub)
                : section + "." + QString::number(sub);
            collectAttachments(p.left(ps),
                               p.mid(ps + (p.at(ps) == '\r' ? 4 : 2)),
                               childSection, nextIdx, atts);
        }
        return;
    }
    // 非 multipart part：判定是否为附件
    bool isAtt = false;
    QString fname;
    // Content-Disposition: attachment (强信号) + 提取 filename
    QRegularExpression cdRe(
        "^Content-Disposition:[ \\t]*attachment[^\\r\\n]*",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    auto cd = cdRe.match(headers);
    if (cd.hasMatch()) {
        isAtt = true;
        // 用 extractDecodedFilename：优先 RFC5987(filename*)，回退 RFC2047(filename=)，
        // 再回退 Content-Type name=
        fname = extractDecodedFilename(headers, ctype);
    }
    // 启发：非文本/非 multipart/非 message → 附件（image/png / application/pdf 等）
    if (!isAtt
        && !ctype.contains("text/", Qt::CaseInsensitive)
        && !ctype.contains("multipart/", Qt::CaseInsensitive)
        && !ctype.contains("message/", Qt::CaseInsensitive)
        && !ctype.isEmpty()) {
        isAtt = true;
    }
    if (!isAtt) return;

    if (fname.isEmpty()) {
        fname = extractDecodedFilename(headers, ctype);
    }
    if (fname.isEmpty()) {
        const int sl = ctype.indexOf('/');
        const int se = (sl >= 0) ? ctype.indexOf(';', sl) : -1;
        QString sub = (sl >= 0) ? ctype.mid(sl + 1,
            (se > sl) ? se - sl - 1 : ctype.size() - sl - 1).trimmed() : QString();
        fname = sub.isEmpty() ? QStringLiteral("attachment") : QStringLiteral("attachment.") + sub;
    }
    if (fname.isEmpty()) {
        const int sl = ctype.indexOf('/');
        const int se = (sl >= 0) ? ctype.indexOf(';', sl) : -1;
        QString sub = (sl >= 0) ? ctype.mid(sl + 1,
            (se > sl) ? se - sl - 1 : ctype.size() - sl - 1).trimmed() : QString();
        fname = sub.isEmpty() ? QStringLiteral("attachment") : QStringLiteral("attachment.") + sub;
    }

    MailStore::Attachment a;
    a.name = fname;
    a.mimeType = ctype.section(';', 0, 0).trimmed();
    a.encoding = cte.trimmed().toLower();
    const qint64 rawSize = body.size();
    if (a.encoding == "base64") a.size = rawSize * 3 / 4;
    else                         a.size = rawSize;     // QP ≈ 1:1；7bit/8bit/binary 原字节
    a.section = section.isEmpty() ? "1" : section;
    atts->append(a);
}

// 从 RFC822 邮件中抽取 text/plain 部分（顶层入口）
void extractBodyParts(const QString& raw, QString* plainOut, QString* htmlOut) {
    if (plainOut) plainOut->clear();
    if (htmlOut)  htmlOut->clear();
    // 找 header/body 分隔
    int sep = raw.indexOf("\r\n\r\n");
    if (sep < 0) sep = raw.indexOf("\n\n");
    if (sep < 0) {
        Logger::instance().warn("extractBodyParts: 找不到 header/body 分隔符，整封邮件当 plain", "mail");
        if (plainOut) *plainOut = raw;
        return;
    }
    const QString topHeaders = raw.left(sep);
    const QString topCtype = extractHeader(topHeaders, "Content-Type").toLower();
    // 全局统计邮件源结构（即便后续解析失败，也能知道这封邮件"长什么样"）
    static const QRegularExpression multipRe("multipart/[\\w-]+",QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tPlainRe("text/plain[^\\r\\n;\"']*", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tHtmlRe( "text/html[^\\r\\n;\"']*", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression cteRe("content-transfer-encoding:[ \\t]*([\\r\\n]+|$)",QRegularExpression::CaseInsensitiveOption);
    const int nMulti = raw.count(multipRe);
    const int nPlain = raw.count(tPlainRe);
    const int nHtml  = raw.count(tHtmlRe);
    Logger::instance().info(
        QString("extractBodyParts: rawLen=%1 topCtype=\"%2\" multipartHits=%3 textPlainHits=%4 textHtmlHits=%5")
            .arg(raw.size())
            .arg(topCtype.left(80))
            .arg(nMulti).arg(nPlain).arg(nHtml),
        "mail");
    collectBodyParts(topHeaders,
                     raw.mid(sep + (raw.at(sep) == '\r' ? 4 : 2)),
                     plainOut, htmlOut);
    Logger::instance().info(
        QString("extractBodyParts: result plainLen=%1 htmlLen=%2")
            .arg(plainOut ? plainOut->size() : -1)
            .arg(htmlOut  ? htmlOut->size()  : -1),
        "mail");
    // 兜底：顶层缺 Content-Type 但正文是 MIME part 结构的畸形邮件。
    // 某些 MTA/转发网关不写顶层 Content-Type 头，正文却直接以完整 part 结构出现
    // （"--boundary\nContent-Type: text/plain ...\n\n正文..."），导致上面的解析为空。
    // 仅当正常路径（含上面的顶层 collectBodyParts）都没收到时，尝试把正文里第一个
    // multipart 声明当作根重新解析；不影响任何正常路径。
    if ((!plainOut || plainOut->isEmpty()) && (!htmlOut || htmlOut->isEmpty())
        && nMulti > 0) {
        const QString theBody = raw.mid(sep + (raw.at(sep) == QLatin1Char('\r') ? 4 : 2));
        static const QRegularExpression mulCtRe(
            "^Content-Type:\\s*multipart/[\\w-]+[^\\r\\n]*(?:\\r?\\n[ \\t][^\\r\\n]*)*",
            QRegularExpression::CaseInsensitiveOption |
            QRegularExpression::MultilineOption);
        auto mc = mulCtRe.match(theBody);
        if (mc.hasMatch()) {
            QString ctHdr = mc.captured();
            QString sub  = theBody.mid(mc.capturedEnd());
            // 收缩到第一个 boundary 分隔行之前，交给 collectBodyParts 处理其补前缀逻辑
            static const QRegularExpression bndRe(
                "boundary=\"?([^\"\\s;]+)\"?",
                QRegularExpression::CaseInsensitiveOption);
            auto bm = bndRe.match(ctHdr);
            if (bm.hasMatch()) {
                const QString bnd = bm.captured(1);
                int bp = sub.indexOf(QStringLiteral("\n--") + bnd);
                if (bp > 0) sub = sub.mid(bp);
            }
            collectBodyParts(ctHdr, sub, plainOut, htmlOut);
            const bool okNow = (!plainOut || !plainOut->isEmpty())
                            || (!htmlOut  || !htmlOut->isEmpty());
            Logger::instance().info(
                QString("extractBodyParts: 顶层缺 Content-Type 兜底解析%1 (plain=%2 html=%3)")
                    .arg(okNow ? QStringLiteral("成功") : QStringLiteral("仍未命中（残缺 part）"))
                    .arg(plainOut ? plainOut->size() : -1)
                    .arg(htmlOut  ? htmlOut->size()  : -1),
                "mail");
        }
    }
    // 解析失败：raw 已拉取但 plain/html 都为空。主动把邮件源前字节 dump 到日志，
    // 用户可直接复制贴给开发者，无需手动另存 .eml。
    const bool noPlain = !plainOut || plainOut->isEmpty();
    const bool noHtml  = !htmlOut  || htmlOut->isEmpty();
    if (noPlain && noHtml) {
        const QByteArray dump = raw.left(4096).toLatin1();
        Logger::instance().warn(
            QString("extractBodyParts: 解析失败 rawLen=%1 topCtype=\"%2\" "
                    "multipartHits=%3 textPlainHits=%4 textHtmlHits=%5 — "
                    "rawSource 前 4096 字节 dump：\n--BEGIN RAW--\n%6\n--END RAW--")
                .arg(raw.size())
                .arg(topCtype.left(120))
                .arg(nMulti).arg(nPlain).arg(nHtml)
                .arg(QString::fromLatin1(dump)),
            "mail");
    }
}

// 纯文本提取（列表摘要等场景）：text/plain 优先，回退 text/html 剥标签
QString extractTextBody(const QString& raw) {
    QString plain, html;
    extractBodyParts(raw, &plain, &html);
    if (!plain.isEmpty()) return plain;
    if (!html.isEmpty())  return htmlToPlainText(html);
    return plain;
}

// ── IMAP ENVELOPE 解析器 ─────────────────────────────────────────────
// 在原始字节流上做递归下降解析。支持：括号、quoted string（含 \" 转义）、
// {N}\r\n literal（按字节数读取）、NIL、atom。
// 用途：UID FETCH (UID FLAGS ENVELOPE) 一次拉取邮件列表元数据，
//      避开 139 等服务器对 BODY[...] 类 FETCH 的严格限流。

struct EnvValue {
    enum Kind { Nil, Str, List } kind = Nil;
    QString str;
    QList<EnvValue> list;
};

static bool envReadValue(const QByteArray& s, int& pos, EnvValue* out) {
    auto skipWs = [&]() {
        while (pos < s.size() && (s[pos]==' ' || s[pos]=='\r' || s[pos]=='\n' || s[pos]=='\t'))
            ++pos;
    };
    skipWs();
    if (pos >= s.size()) return false;
    char c = s[pos];

    if (c == '(') {                       // 括号结构 → 递归读成员
        ++pos;
        out->kind = EnvValue::List;
        out->list.clear();
        while (true) {
            skipWs();
            if (pos >= s.size()) return false;
            if (s[pos] == ')') { ++pos; return true; }
            EnvValue v;
            if (!envReadValue(s, pos, &v)) return false;
            out->list.append(v);
        }
    }
    if (c == '"') {                       // quoted string（处理 \" 转义）
        ++pos;
        QString result;
        while (pos < s.size()) {
            if (s[pos] == '\\' && pos + 1 < s.size()) {
                result += QChar(s[pos + 1]);
                pos += 2;
                continue;
            }
            if (s[pos] == '"') {
                ++pos;
                out->kind = EnvValue::Str;
                out->str = result;
                return true;
            }
            result += QChar(s[pos]);
            ++pos;
        }
        return false;
    }
    if (c == '{') {                       // literal {N}\r\n + N 字节
        int braceEnd = s.indexOf('}', pos);
        if (braceEnd < 0) return false;
        bool ok = false;
        int n = s.mid(pos + 1, braceEnd - pos - 1).toInt(&ok);
        if (!ok || n < 0) return false;
        pos = braceEnd + 1;
        if (pos < s.size() && s[pos] == '\r') ++pos;
        if (pos < s.size() && s[pos] == '\n') ++pos;
        if (pos + n > s.size()) n = s.size() - pos;   // 截断保护
        out->kind = EnvValue::Str;
        out->str = QString::fromUtf8(s.mid(pos, n));
        pos += n;
        return true;
    }
    // NIL 或 atom（含 \Seen 这类 flag）
    int start = pos;
    while (pos < s.size() && s[pos] != ' ' && s[pos] != '\r' && s[pos] != '\n'
           && s[pos] != ')' && s[pos] != '(') {
        ++pos;
    }
    if (pos == start) return false;
    QByteArray atom = s.mid(start, pos - start);
    out->kind = (atom == "NIL") ? EnvValue::Nil : EnvValue::Str;
    out->str = QString::fromUtf8(atom);
    return true;
}

// 从原始响应字节流解析所有 "* n FETCH (UID x FLAGS (...) ENVELOPE (...))"
// 即使 tagged OK 未到（139 限流 hold），已收到的 untagged 数据也能解析。
static void parseEnvelopeResponses(const QByteArray& raw,
                                   QList<ImapClient::FetchedMessage>* out) {
    int searchFrom = 0;
    while (true) {
        int fpos = raw.indexOf(" FETCH (", searchFrom);
        if (fpos < 0) break;
        searchFrom = fpos + 1;
        // 验证所在行是 untagged 响应（"* n FETCH ("）
        int lineStart = raw.lastIndexOf('\n', fpos);
        lineStart = (lineStart < 0) ? 0 : lineStart + 1;
        QByteArray prefix = raw.mid(lineStart, fpos - lineStart).trimmed();
        if (!prefix.startsWith('*')) continue;

        int vpos = fpos + 7;              // 指向 '(' 本身，envReadValue 读整个括号结构
        EnvValue fv;
        if (!envReadValue(raw, vpos, &fv) || fv.kind != EnvValue::List) continue;

        // FETCH 括号内是 key value 序列：UID n FLAGS (...) ENVELOPE (...)
        QString uid;
        bool seen = false, hasEnv = false;
        EnvValue env;
        QString internalDate;
        for (int i = 0; i + 1 < fv.list.size(); i += 2) {
            QString key = fv.list[i].str.toUpper();
            const EnvValue& v = fv.list[i + 1];
            if (key == "UID" && v.kind == EnvValue::Str) {
                uid = v.str;
            } else if (key == "FLAGS" && v.kind == EnvValue::List) {
                for (const auto& f : v.list) {
                    if (f.kind == EnvValue::Str &&
                        f.str.contains("\\Seen", Qt::CaseInsensitive))
                        seen = true;
                }
            } else if (key == "ENVELOPE" && v.kind == EnvValue::List) {
                env = v;
                hasEnv = true;
            } else if (key == "INTERNALDATE" && v.kind == EnvValue::Str) {
                internalDate = v.str;
            }
        }
        if (!hasEnv || env.list.size() < 10) continue;

        // ENVELOPE (date subject from sender reply-to to cc bcc in-reply-to message-id)
        const auto& L = env.list;
        ImapClient::FetchedMessage fm;
        fm.imapUid = uid;
        // messageId 留空：等 ENVELOPE 解析（L[9]）拿到真值再填；
        // 旧逻辑用 "uid:xxx" 占位会让预览误把 IMAP UID 显示成 Message-ID
        fm.messageId = QString();
        fm.seen = seen;
        QString dateStr = (L[0].kind == EnvValue::Str) ? L[0].str : QString();
        fm.subject = (L[1].kind == EnvValue::Str) ? decodeMime(L[1].str) : QString();

        // 地址结构：(name adl mailbox host)；email = mailbox@host
        auto firstAddr = [](const EnvValue& v) -> QString {
            if (v.kind != EnvValue::List || v.list.isEmpty()) return {};
            const EnvValue& a = v.list.first();
            if (a.kind != EnvValue::List || a.list.size() < 4) return {};
            QString name    = (a.list[0].kind == EnvValue::Str) ? decodeMime(a.list[0].str) : QString();
            QString mailbox = (a.list[2].kind == EnvValue::Str) ? a.list[2].str : QString();
            QString host    = (a.list[3].kind == EnvValue::Str) ? a.list[3].str : QString();
            QString email = mailbox + (host.isEmpty() ? QString() : "@" + host);
            if (email.isEmpty()) return {};
            return name.isEmpty() ? email : QString("%1 <%2>").arg(name, email);
        };
        auto allAddrs = [](const EnvValue& v) -> QStringList {
            QStringList res;
            if (v.kind != EnvValue::List) return res;
            for (const auto& a : v.list) {
                if (a.kind != EnvValue::List || a.list.size() < 4) continue;
                QString mailbox = (a.list[2].kind == EnvValue::Str) ? a.list[2].str : QString();
                QString host    = (a.list[3].kind == EnvValue::Str) ? a.list[3].str : QString();
                if (!mailbox.isEmpty())
                    res.append(mailbox + (host.isEmpty() ? QString() : "@" + host));
            }
            return res;
        };
        fm.from = firstAddr(L[2]);
        fm.to   = allAddrs(L[5]);
        fm.cc   = allAddrs(L[6]);
        QString mid = (L[9].kind == EnvValue::Str) ? L[9].str.trimmed() : QString();
        if (!mid.isEmpty()) fm.messageId = mid;
        // 日期：ENVELOPE date（139 常返回 NIL）→ INTERNALDATE → 当前时间
        fm.date = QDateTime::fromString(dateStr, Qt::RFC2822Date);
        if (!fm.date.isValid() && !internalDate.isEmpty()) {
            // INTERNALDATE 格式 "15-Sep-2026 10:00:00 +0800" → RFC2822 兼容
            QString idate = internalDate;
            idate.replace(QRegularExpression("^(\\d+)-([A-Za-z]{3})-"), "\\1 \\2 ");
            fm.date = QDateTime::fromString(idate, Qt::RFC2822Date);
        }
        if (!fm.date.isValid()) fm.date = QDateTime::currentDateTime();
        out->append(fm);
    }
}

} // namespace

bool ImapClient::fetchUnread(const Config& c,
                             QList<FetchedMessage>* out,
                             QString* errorMessage,
                             int maxCount) {
    out->clear();
    int timeoutMs = c.timeoutSec * 1000;
    if (maxCount <= 0) maxCount = 5;   // 默认 5 封(139 限速严格,30/10 都超时)

    Logger::instance().info(
        QString("IMAP.connect: %1:%2 ssl=%3 user=%4 timeout=%5s")
            .arg(c.host).arg(c.port).arg(c.ssl ? "Y" : "N")
            .arg(c.username).arg(c.timeoutSec),
        "mail");

    QSslSocket sock;
    QObject::connect(&sock, &QSslSocket::sslErrors, &sock, [&sock](const QList<QSslError>&) {
        sock.ignoreSslErrors();
    });

    if (c.ssl) {
        sock.connectToHostEncrypted(c.host, static_cast<quint16>(c.port));
        if (!sock.waitForEncrypted(timeoutMs)) {
            QString err = "IMAP SSL 连接失败: " + sock.errorString();
            Logger::instance().warn(err, "mail");
            if (errorMessage) *errorMessage = err;
            return false;
        }
    } else {
        sock.connectToHost(c.host, static_cast<quint16>(c.port));
        if (!sock.waitForConnected(timeoutMs)) {
            QString err = "IMAP 连接失败: " + sock.errorString();
            Logger::instance().warn(err, "mail");
            if (errorMessage) *errorMessage = err;
            return false;
        }
    }
    Logger::instance().info("IMAP.connect: 已建立连接", "mail");

    QStringList resp;
    QString status;
    QString err;

    // 读欢迎语（IMAP server 会发 untagged "* OK ..."，没有 <tag> 终止行）
    // 这里不能用 readResponse(sock, "S0", ...)，因为它会一直等 "S0 OK" 而
    // 永远不会等到,白白消耗 timeoutMs（120s）。直接读 1 行即可。
    Tag tag;
    QString welcome;
    if (readLine(sock, timeoutMs, &welcome)) {
        Logger::instance().info(
            QString("IMAP 欢迎语: %1").arg(welcome.left(120)), "mail");
    } else {
        Logger::instance().warn(
            "IMAP 未收到欢迎语,继续尝试 LOGIN", "mail");
    }

    // LOGIN
    if (!sendCmd(sock, tag,
            QString("LOGIN \"%1\" \"%2\"").arg(c.username, c.password),
            timeoutMs, &resp, &status, &err)) {
        if (errorMessage) *errorMessage = "IMAP 登录失败: " + status;
        sock.write("A001 LOGOUT\r\n"); sock.waitForBytesWritten(2000);
        sock.disconnectFromHost();
        return false;
    }

    // SELECT INBOX
    if (!sendCmd(sock, tag, "SELECT INBOX", timeoutMs, &resp, &status, &err)) {
        if (errorMessage) *errorMessage = "IMAP SELECT 失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }

    // UID SEARCH UNSEEN
    if (!sendCmd(sock, tag, "UID SEARCH UNSEEN", timeoutMs, &resp, &status, &err)) {
        if (errorMessage) *errorMessage = "IMAP SEARCH 失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }

    QStringList uids;
    for (const QString& l : resp) {
        if (l.startsWith("* SEARCH")) {
            for (const QString& s : l.mid(9).trimmed().split(' ', Qt::SkipEmptyParts)) {
                uids.append(s);
            }
        }
    }
    Logger::instance().info(
        QString("IMAP SEARCH UNSEEN: 命中 %1 个 UID (限制最多 %2 封最新)")
            .arg(uids.size()).arg(maxCount),
        "mail");
    // 仅保留最新 maxCount 个 UID(UID 大的一般是新的),避免 139 IMAP 对大量
    // UID batch FETCH 限速导致超时
    if (uids.size() > maxCount) {
        uids = uids.mid(uids.size() - maxCount);
        Logger::instance().info(
            QString("IMAP SEARCH UNSEEN 截断到 %1 封最新 UID").arg(uids.size()),
            "mail");
    }
    if (uids.isEmpty()) {
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return true;
    }

    // 一条 batch ENVELOPE FETCH 拉全部未读元数据。
    // 历史教训：BODY.PEEK[HEADER.FIELDS] 类 FETCH 被 139 限流（tagged OK
    // hold 60s+，batch 5 封也超时），ENVELOPE 是服务器元数据缓存不受限。
    QElapsedTimer fetcher; fetcher.start();
    QByteArray rawStream;
    QString fetchCmd = QString("UID FETCH %1 (UID FLAGS ENVELOPE INTERNALDATE)").arg(uids.join(','));
    bool fetchOk = sendCmd(sock, tag, fetchCmd, timeoutMs, &resp, &status, &err, &rawStream);
    // 超时容错：tagged OK 被 hold 时 untagged 数据通常已到，照常解析
    parseEnvelopeResponses(rawStream, out);
    if (out->isEmpty()) {
        Logger::instance().warn(
            QString("IMAP ENVELOPE FETCH 调试: raw=%1 字节, 前 400 字节=[%2]")
                .arg(rawStream.size())
                .arg(QString::fromUtf8(rawStream.left(400))),
            "mail");
    }
    Logger::instance().info(
        QString("IMAP ENVELOPE FETCH[INBOX]: 请求 %1 UID, 解析 %2 封, ok=%3, 耗时 %4ms")
            .arg(uids.size()).arg(out->size())
            .arg(fetchOk ? "Y" : "N").arg(fetcher.elapsed()),
        "mail");

    Logger::instance().info(
        QString("IMAP 拉取完成: host=%1 收到 %2 封 耗时 %3ms")
            .arg(c.host).arg(out->size()).arg(fetcher.elapsed()),
        "mail");

    sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
    sock.disconnectFromHost();

    Logger::instance().info(QString("IMAP 拉取完成: %1 封 (host=%2)")
        .arg(out->size()).arg(c.host), "mail");
    return true;
}

bool ImapClient::listFolders(const Config& c, QList<Folder>* out, QString* errorMessage) {
    out->clear();
    int timeoutMs = c.timeoutSec * 1000;
    QSslSocket sock;
    QObject::connect(&sock, &QSslSocket::sslErrors, &sock, [&sock](const QList<QSslError>&) {
        sock.ignoreSslErrors();
    });
    if (c.ssl) {
        sock.connectToHostEncrypted(c.host, static_cast<quint16>(c.port));
        if (!sock.waitForEncrypted(timeoutMs)) {
            if (errorMessage) *errorMessage = "IMAP SSL 连接失败";
            return false;
        }
    } else {
        sock.connectToHost(c.host, static_cast<quint16>(c.port));
        if (!sock.waitForConnected(timeoutMs)) {
            if (errorMessage) *errorMessage = "IMAP 连接失败";
            return false;
        }
    }
    Tag tag;
    QStringList resp; QString status; QString err;
    sendCmd(sock, tag, QString("LOGIN \"%1\" \"%2\"").arg(c.username, c.password),
            timeoutMs, &resp, &status, &err);
    if (!sendCmd(sock, tag, "LIST \"\" \"*\"", timeoutMs, &resp, &status, &err)) {
        if (errorMessage) *errorMessage = "IMAP LIST 失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }
    // 兼容：分隔符为 NIL；文件夹名为引号串或裸 atom
    static const QRegularExpression re(
        "^\\* LIST \\(([^)]*)\\)\\s+(?:\"([^\"]*)\"|NIL)\\s+(?:\"([^\"]+)\"|(\\S+))");
    for (const QString& l : resp) {
        auto m = re.match(l);
        if (m.hasMatch()) {
            Folder f;
            f.flags     = m.captured(1);
            f.delimiter = m.captured(2);
            f.name      = !m.captured(3).isEmpty() ? m.captured(3) : m.captured(4);
            out->append(f);
        }
    }
    sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
    sock.disconnectFromHost();
    Logger::instance().info(QString("IMAP LIST: %1 个文件夹 (host=%2)")
        .arg(out->size()).arg(c.host), "mail");
    return true;
}

QString ImapClient::decodeFolderName(const QString& in) {
    // IMAP modified UTF-7：& 开头、- 结尾，中间是 UTF-16BE 的 Base64（',' 替代 '/'）
    QString out;
    out.reserve(in.size());
    int i = 0;
    while (i < in.size()) {
        if (in[i] != '&') { out += in[i]; ++i; continue; }
        int end = in.indexOf('-', i + 1);
        if (end < 0) { out += in[i]; ++i; continue; }
        if (end == i + 1) { out += '&'; i = end + 1; continue; }  // "&-" 表示字面 '&'
        QByteArray b64;
        for (int j = i + 1; j < end; ++j) {
            char ch = in[j].toLatin1();
            b64 += (ch == ',') ? '/' : ch;
        }
        while (b64.size() % 4) b64 += '=';
        QByteArray raw = QByteArray::fromBase64(b64);
        for (int k = 0; k + 1 < raw.size(); k += 2) {
            out += QChar(static_cast<char16_t>(
                (static_cast<quint16>(static_cast<uchar>(raw[k])) << 8)
                | static_cast<quint16>(static_cast<uchar>(raw[k + 1]))));
        }
        i = end + 1;
    }
    return out;
}

bool ImapClient::fetchHeaders(const Config& c, const QString& folder, int maxCount,
                              QList<FetchedMessage>* out, QString* errorMessage,
                              int* totalInFolder) {
    // 拉取文件夹邮件头（列表展示用）
    // 关键：不再逐封 FETCH BODY.PEEK[HEADER.FIELDS] —— 139 等服务器对
    // BODY[...] 类 FETCH 严格限流（tagged OK 被 hold 60s+，50 封逐封拉
    // 实测全部超时）。改为一条 batch 命令拉 ENVELOPE 元数据（服务器
    // 内存缓存，不受限流影响），一次响应拿全部列表字段。
    out->clear();
    int timeoutMs = c.timeoutSec * 1000;
    QSslSocket sock;
    QObject::connect(&sock, &QSslSocket::sslErrors, &sock, [&sock](const QList<QSslError>&) {
        sock.ignoreSslErrors();
    });
    if (c.ssl) {
        sock.connectToHostEncrypted(c.host, static_cast<quint16>(c.port));
        if (!sock.waitForEncrypted(timeoutMs)) {
            if (errorMessage) *errorMessage = "IMAP SSL 连接失败: " + sock.errorString();
            return false;
        }
    } else {
        sock.connectToHost(c.host, static_cast<quint16>(c.port));
        if (!sock.waitForConnected(timeoutMs)) {
            if (errorMessage) *errorMessage = "IMAP 连接失败: " + sock.errorString();
            return false;
        }
    }
    // 读欢迎语（untagged，直接读 1 行，不能等 tagged 终止行）
    Tag tag;
    QString welcome;
    if (!readLine(sock, timeoutMs, &welcome)) {
        Logger::instance().warn("IMAP 未收到欢迎语,继续尝试 LOGIN", "mail");
    }
    QStringList resp; QString status; QString err;
    if (!sendCmd(sock, tag, QString("LOGIN \"%1\" \"%2\"").arg(c.username, c.password),
                 timeoutMs, &resp, &status, &err)) {
        if (errorMessage) *errorMessage = "IMAP 登录失败: " + status;
        sock.disconnectFromHost();
        return false;
    }
    QString selFolder = folder.isEmpty() ? "INBOX" : folder;
    if (!sendCmd(sock, tag, "SELECT \"" + selFolder + "\"", timeoutMs, &resp, &status, &err)) {
        if (errorMessage) *errorMessage = "IMAP SELECT 失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }
    // 取最新 maxCount 封：UID SEARCH ALL，然后排序取后 N
    if (!sendCmd(sock, tag, "UID SEARCH ALL", timeoutMs, &resp, &status, &err)) {
        if (errorMessage) *errorMessage = "IMAP SEARCH 失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }
    QStringList allUids;
    for (const QString& l : resp) {
        if (l.startsWith("* SEARCH")) {
            for (const QString& s : l.mid(9).trimmed().split(' ', Qt::SkipEmptyParts)) allUids.append(s);
        }
    }
    if (totalInFolder) *totalInFolder = allUids.size();
    // 取尾部 maxCount 个（UID 大的一般是新的）
    // 取尾部 maxCount 个（UID 大的一般是新的）
    if (allUids.size() > maxCount) {
        allUids = allUids.mid(allUids.size() - maxCount);
    }
    if (allUids.isEmpty()) {
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return true;
    }

    // 一条 batch ENVELOPE FETCH 拉全部（含 FLAGS，用于已读状态）
    QByteArray rawStream;
    QString cmd = QString("UID FETCH %1 (UID FLAGS ENVELOPE INTERNALDATE)").arg(allUids.join(','));
    QElapsedTimer fetcher; fetcher.start();
    bool ok = sendCmd(sock, tag, cmd, timeoutMs, &resp, &status, &err, &rawStream);
    // 超时容错：139 等服务器限流时 tagged OK 被 hold，但 untagged FETCH
    // 数据（含 literal）通常已到 —— 已收到的数据仍然解析，能拿多少算多少
    parseEnvelopeResponses(rawStream, out);
    Logger::instance().info(
        QString("IMAP ENVELOPE FETCH[%1]: 请求 %2 UID, 解析 %3 封, ok=%4, 耗时 %5ms")
            .arg(selFolder).arg(allUids.size()).arg(out->size())
            .arg(ok ? "Y" : "N").arg(fetcher.elapsed()),
        "mail");
    if (out->isEmpty() && !ok) {
        if (errorMessage) *errorMessage = "IMAP FETCH 失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }
    sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
    sock.disconnectFromHost();
    return true;
}

// ── 辅助：通用 connect+login+select ──
static bool imapOpen(QSslSocket& sock, const ImapClient::Config& c,
                     const QString& folder, Tag& tag,
                     QStringList* resp, QString* status, QString* err,
                     QString* errorMessage) {
    int timeoutMs = c.timeoutSec * 1000;
    QObject::connect(&sock, &QSslSocket::sslErrors, &sock, [&sock](const QList<QSslError>&) {
        sock.ignoreSslErrors();
    });
    if (c.ssl) {
        Logger::instance().info("IMAP SSL: connectToHostEncrypted", "mail");
        sock.connectToHostEncrypted(c.host, c.port);
        Logger::instance().info("IMAP SSL: waitForEncrypted 开始", "mail");
        if (!sock.waitForEncrypted(timeoutMs)) {
            Logger::instance().warn("IMAP SSL: waitForEncrypted 超时或失败: " + sock.errorString(), "mail");
            if (errorMessage) *errorMessage = "IMAP 连接失败: " + sock.errorString();
            return false;
        }
        Logger::instance().info("IMAP SSL: 已建立加密连接", "mail");
    } else {
        Logger::instance().info("IMAP TCP: connectToHost", "mail");
        sock.connectToHost(c.host, c.port);
        Logger::instance().info("IMAP TCP: waitForConnected 开始", "mail");
        if (!sock.waitForConnected(timeoutMs)) {
            Logger::instance().warn("IMAP TCP: waitForConnected 超时或失败: " + sock.errorString(), "mail");
            if (errorMessage) *errorMessage = "IMAP 连接失败: " + sock.errorString();
            return false;
        }
        Logger::instance().info("IMAP TCP: 已建立连接", "mail");
    }
    Logger::instance().info("IMAP: 读 server greeting", "mail");
    // server 推送的是 untagged "* OK ..."，没有 <tag> 终止行，不能用 readResponse
    // 等 "<tag> OK"，否则会白白等 timeoutMs。直接读 1 行。
    QString welcome;
    if (readLine(sock, timeoutMs, &welcome)) {
        Logger::instance().info(
            QString("IMAP 欢迎语: %1").arg(welcome.left(120)), "mail");
    } else {
        Logger::instance().warn("IMAP 未收到欢迎语,继续尝试 LOGIN", "mail");
    }
    Logger::instance().info(QString("IMAP: 发送 LOGIN"), "mail");
    if (!sendCmd(sock, tag, QString("LOGIN \"%1\" \"%2\"").arg(c.username, c.password),
                 timeoutMs, resp, status, err)) {
        if (errorMessage) *errorMessage = "IMAP 登录失败: " + *status;
        return false;
    }
    QString sel = folder.isEmpty() ? "INBOX" : folder;
    if (!sendCmd(sock, tag, "SELECT \"" + sel + "\"", timeoutMs, resp, status, err)) {
        if (errorMessage) *errorMessage = "IMAP SELECT 失败: " + *status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, resp, status, nullptr);
        return false;
    }
    return true;
}

bool ImapClient::fetchBody(const Config& c, const QString& folder,
                           const QString& imapUid,
                           QString* body, QString* htmlBody,
                           QByteArray* rawSource,
                           QList<MailStore::Attachment>* attsOut,
                           QString* errorMessage,
                           const FetchProgressCb& onProgress) {
    if (body) body->clear();
    if (htmlBody) htmlBody->clear();
    if (rawSource) rawSource->clear();
    if (attsOut) attsOut->clear();
    int timeoutMs = c.timeoutSec * 1000;
    QSslSocket sock;
    Tag tag;
    QStringList resp; QString status; QString err;
    if (!imapOpen(sock, c, folder, tag, &resp, &status, &err, errorMessage))
        return false;

    // UID FETCH <uid> BODY.PEEK[] —— 拉整封 MIME 原文（含全部 header + part header）。
    // 旧实现 BODY.PEEK[TEXT] 只拿 body 段且无 part header，编码问题无解：
    //   - multipart 邮件显示 boundary + HTML 源码 + base64 附件乱码
    //   - charset / CTE 只能嗅探：正则误判 QP（正文含 "=xx" 即破坏）、
    //     base64 正文不解码、8bit GBK 靠猜
    // 改为整封拉取后走 extractTextBody 完整 MIME 解析：multipart 按 boundary
    // 拆分取 text/plain（回退 text/html 剥标签），每 part 按其 header 声明的
    // charset + Content-Transfer-Encoding 精确解码。
    QByteArray rawStream;
    if (!sendCmd(sock, tag,
                 QString("UID FETCH %1 BODY.PEEK[]").arg(imapUid),
                 timeoutMs, &resp, &status, &err, &rawStream,
                 onProgress ? onProgress : FetchProgressCb(nullptr))) {
        if (errorMessage) *errorMessage = "IMAP FETCH 正文失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }
    // 在原始字节流中定位 "BODY[] {N}\r\n<N 字节>"（rawStream 保 8-bit 无损；
    // "BODY[]" 不会误匹配 "BODY[TEXT]" 等带段名的变体）
    for (int bp = rawStream.indexOf("BODY[]"); bp >= 0;
         bp = rawStream.indexOf("BODY[]", bp + 1)) {
        int ob = rawStream.indexOf('{', bp);
        int cb = ob >= 0 ? rawStream.indexOf('}', ob) : -1;
        if (cb <= ob) continue;
        bool ok = false;
        int len = rawStream.mid(ob + 1, cb - ob - 1).toInt(&ok);
        int start = cb + 1;
        if (start < rawStream.size() && rawStream[start] == '\r') ++start;
        if (start < rawStream.size() && rawStream[start] == '\n') ++start;
        if (!ok || len < 0 || start + len > rawStream.size()) continue;
        if (body || htmlBody || rawSource || attsOut) {
            QByteArray lit = rawStream.mid(start, len);
            if (rawSource) *rawSource = lit;
            // Latin-1 保字节入 QString（header/boundary/QP/base64 均为 ASCII，
            // 8bit 正文字节由 part 声明的 charset 在 extractBodyParts 内还原）
            const QString sLit = QString::fromLatin1(lit);
            if (body || htmlBody) {
                QString plain, html;
                extractBodyParts(sLit, &plain, &html);
                if (body) *body = !plain.isEmpty() ? plain : htmlToPlainText(html);
                if (htmlBody) *htmlBody = html;
            }
            if (attsOut) {
                const int sep = sLit.indexOf("\r\n\r\n");
                if (sep >= 0) {
                    int nextIdx = 1;
                    collectAttachments(sLit.left(sep), sLit.mid(sep + 4),
                                       QString(), &nextIdx, attsOut);
                }
            }
        }
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return true;
    }
    if (errorMessage) *errorMessage = "IMAP FETCH 正文响应格式异常";
    sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
    sock.disconnectFromHost();
    return false;
}

// ── 预览加速快路径：只拉正文（附件仅取元数据） ───────────────────────────
// 用 BODYSTRUCTURE 解析整封 MIME 树，仅对"纯 正文+普通附件"邮件生效：
//   - 正文 text/plain / text/html part → 按 section 拉取并解码
//   - 附件只从 BODYSTRUCTURE 生成元数据（filename/mime/encoding/size/section），不下载内容
// 以下结构返回 false，调用方回退 fetchBody 整封拉（保证 cid 内嵌图 / 嵌套邮件 / 签名正确）：
//   - 任一 part 带 Content-ID（内嵌 cid 图片，需 rawSource 才能解析）
//   - 含 message/rfc822 嵌套邮件
//   - multipart/signed 或 multipart/encrypted
bool ImapClient::fetchBodyFast(const Config& c, const QString& folder,
                               const QString& imapUid,
                               QString* body, QString* htmlBody,
                               QList<MailStore::Attachment>* attsOut,
                               QString* errorMessage,
                               const FetchProgressCb& onProgress) {
    if (body) body->clear();
    if (htmlBody) htmlBody->clear();
    if (attsOut) attsOut->clear();
    int timeoutMs = c.timeoutSec * 1000;
    QSslSocket sock; Tag tag;
    QStringList resp; QString status; QString err;
    if (!imapOpen(sock, c, folder, tag, &resp, &status, &err, errorMessage)) return false;

    // 拉 BODYSTRUCTURE（结构元数据，无 literal，传输很小）
    QByteArray bsRaw;
    if (!sendCmd(sock, tag,
                 QString("UID FETCH %1 BODYSTRUCTURE").arg(imapUid),
                 timeoutMs, &resp, &status, &err, &bsRaw)) {
        if (errorMessage) *errorMessage = "IMAP 获取 BODYSTRUCTURE 失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }
    int bsIdx = bsRaw.indexOf("BODYSTRUCTURE");
    int open = bsIdx >= 0 ? bsRaw.indexOf('(', bsIdx) : -1;
    EnvValue bs;
    if (open < 0 || !envReadValue(bsRaw, open, &bs) || bs.kind != EnvValue::List) {
        if (errorMessage) *errorMessage = "IMAP BODYSTRUCTURE 解析失败";
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }

    struct TextPart { QString section, enc, charset; bool html = false; };
    QList<TextPart> texts;
    QList<MailStore::Attachment> atts;
    bool complex = false;   // 含内嵌图 / 嵌套邮件 / 签名 → 需回退整封

    // 从 params list（键值对序列）取名为 key 的值
    auto paramOf = [](const EnvValue& any, const QString& key) -> QString {
        if (any.kind != EnvValue::List) return {};
        const auto& P = any.list;
        for (int i = 0; i + 1 < P.size(); i += 2)
            if (P[i].kind == EnvValue::Str && P[i].str.compare(key, Qt::CaseInsensitive) == 0
                && P[i+1].kind == EnvValue::Str)
                return P[i+1].str;
        return {};
    };
    // 从 disposition (如 ("attachment" ("filename" "x.pdf"))) 取
    // 格式：某叶元素的扩展尾巴里有一个 List，其 [0] = "attachment"/"inline"，
    // 其后 [1] 又是一个 List（参数对）。filename 参数通常这里取。
    auto dispositionOf = [&](const EnvValue& any, QString* type) -> QString {
        if (any.kind != EnvValue::List) return {};
        const auto& L = any.list;
        for (const auto& it : L) {
            if (it.kind != EnvValue::List || it.list.isEmpty()) continue;
            const auto& D = it.list;
            if (D[0].kind != EnvValue::Str) continue;
            const QString t = D[0].str.toLower();
            if (t == "attachment" || t == "inline") {
                if (type) *type = t;
                if (D.size() > 1)
                    return paramOf(D[1], QStringLiteral("filename"));
            }
        }
        return {};
    };

    // 递归遍历 BODYSTRUCTURE 树；sectionPrefix 为父容器已归档的 section 前缀
    std::function<void(const EnvValue&, const QString&)> walk;
    walk = [&](const EnvValue& v, const QString& prefix) {
        if (v.kind != EnvValue::List || v.list.isEmpty()) { complex = true; return; }
        const auto& L = v.list;
        if (L[0].kind == EnvValue::Str) {
            // 1-part 叶：("text" "plain" (params) <id> <desc> <enc> <size> ...)
            if (L.size() < 2) { complex = true; return; }
            const QString type    = L[0].str.toLower();
            const QString subtype = L[1].str.toLower();
            // Content-ID（内嵌图片）→ 需 rawSource 才能解析 → 回退
            if (L.size() > 3 && L[3].kind == EnvValue::Str && !L[3].str.isEmpty()) {
                complex = true;
                return;
            }
            if (type == "message") { complex = true; return; }   // 嵌套邮件 rfc822
            const QString enc  = (L.size() > 5 && L[5].kind == EnvValue::Str) ? L[5].str : QString();
            const EnvValue ctypeParams = (L.size() > 2) ? L[2] : EnvValue();
            const QString charset = paramOf(ctypeParams, QStringLiteral("charset"));
            const QString nameParam = paramOf(ctypeParams, QStringLiteral("name"));
            QString dispType; const QString dispFile = dispositionOf(v, &dispType);
            const QString section = prefix.isEmpty() ? QStringLiteral("1") : prefix;

            if (type == "text" && (subtype == "plain" || subtype == "html")) {
                TextPart tp;
                tp.section = section; tp.enc = enc; tp.charset = charset;
                tp.html = (subtype == "html");
                texts.append(tp);
                return;
            }
            // 附件：disposition=attachment 或 非 inline 且非正文文本的多媒体
            if (dispType != "inline") {
                MailStore::Attachment a;
                a.name = dispFile.isEmpty() ? nameParam : dispFile;
                // BODYSTRUCTURE 的 filename/name 参数可能是 RFC2047 编码（=?UTF-8?B?...?=），
                // 需解码；decodeMimeWord 对未编码值原样返回，安全。
                a.name = decodeMimeWord(a.name);
                if (a.name.isEmpty())
                    a.name = QStringLiteral("attachment.%1").arg(subtype);
                a.mimeType = type.isEmpty() ? subtype : type + "/" + subtype;
                a.encoding = enc.toLower();
                if (L.size() > 6 && L[6].kind == EnvValue::Str) {
                    const qint64 oct = L[6].str.toLongLong();
                    a.size = (a.encoding == "base64") ? oct * 3 / 4 : oct;
                }
                a.section = section;
                atts.append(a);
            }
            return;
        }
        // multipart：L[0] 是子 body 的 List；其后是 Str media-subtype 与扩展
        QString mtype;
        int childNo = 0;
        for (const auto& it : L) {
            if (it.kind == EnvValue::Str) { mtype = it.str.toLower(); break; }
            if (it.kind == EnvValue::List) {
                ++childNo;
                const QString childPrefix = prefix.isEmpty()
                    ? QString::number(childNo)
                    : prefix + "." + QString::number(childNo);
                walk(it, childPrefix);
                if (complex) return;
            }
        }
        if (mtype.contains("signed") || mtype.contains("encrypted")) complex = true;
    };
    walk(bs, QString());
    if (complex) {
        if (errorMessage) *errorMessage = "邮件结构复杂(内嵌图/转发/签名)，回退整封拉取";
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }

    // 拉取每个正文 part 并解码（一次连接内连续 FETCH）
    for (const auto& tp : texts) {
        QByteArray rawStream;
        QString s, e;
        const QString cmd = QString("UID FETCH %1 BODY.PEEK[%2]").arg(imapUid, tp.section);
        if (!sendCmd(sock, tag, cmd, timeoutMs, &resp, &s, &e, &rawStream,
                     onProgress ? onProgress : FetchProgressCb(nullptr))) {
            if (errorMessage) *errorMessage = "IMAP 拉取正文 part 失败: " + s;
            sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
            sock.disconnectFromHost();
            return false;
        }
        const QByteArray key = ("BODY[" + tp.section).toLatin1() + "]";
        for (int bp = rawStream.indexOf(key); bp >= 0; bp = rawStream.indexOf(key, bp + 1)) {
            int ob = rawStream.indexOf('{', bp);
            int cb = ob >= 0 ? rawStream.indexOf('}', ob) : -1;
            if (cb <= ob) continue;
            bool ok = false;
            int len = rawStream.mid(ob + 1, cb - ob - 1).toInt(&ok);
            int start = cb + 1;
            if (start < rawStream.size() && rawStream[start] == '\r') ++start;
            if (start < rawStream.size() && rawStream[start] == '\n') ++start;
            if (!ok || len < 0 || start + len > rawStream.size()) continue;
            const QByteArray lit = rawStream.mid(start, len);
            const QString decoded = decodeTransferEncoding(tp.enc.isEmpty() ? "7bit" : tp.enc,
                                                           QString::fromLatin1(lit), tp.charset);
            if (tp.html) { if (htmlBody && htmlBody->isEmpty()) *htmlBody = decoded; }
            else         { if (body    && body->isEmpty())    *body    = decoded; }
            break;
        }
    }

    if (attsOut) *attsOut = atts;
    sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
    sock.disconnectFromHost();
    return true;
}

// ── 拉取单个 MIME part（附件下载） ──
//   命令：UID FETCH <uid> BODY.PEEK[<section>]  →  返回该 part 编码后的字节
//   响应： "* N FETCH (UID <uid> BODY[section] {K}" + K 字节 literal + ")"
//   出参 decoded：按 encoding 解码后的字节（base64/QP/7bit/8bit/binary）
bool ImapClient::fetchPart(const Config& c, const QString& folder,
                           const QString& imapUid, const QString& section,
                           const QString& encoding,
                           QByteArray* decoded, QString* errorMessage) {
    return fetchPart(c, folder, imapUid, section, encoding, decoded, errorMessage, nullptr);
}

bool ImapClient::fetchPart(const Config& c, const QString& folder,
                           const QString& imapUid, const QString& section,
                           const QString& encoding,
                           QByteArray* decoded, QString* errorMessage,
                           const FetchProgressCb& onProgress) {
    if (decoded) decoded->clear();
    int timeoutMs = c.timeoutSec * 1000;
    QSslSocket sock;
    Tag tag;
    QStringList resp; QString status; QString err;
    if (!imapOpen(sock, c, folder, tag, &resp, &status, &err, errorMessage))
        return false;

    // 立即报一次 0% —— 让 UI 立刻从"点击下载"切到"下载中 0%"
    if (onProgress) onProgress(0, 0);

    const QString cmd = QString("UID FETCH %1 BODY.PEEK[%2]").arg(imapUid, section);
    QByteArray rawStream;
    // 接收 literal 传输阶段即按块上报进度（对 base64/QP 附件，这才是真正的下载进度，
    // 否则解码只能整段一次、进度只会 0%→100% 跳变）
    if (!sendCmd(sock, tag, cmd, timeoutMs, &resp, &status, &err, &rawStream, onProgress)) {
        if (errorMessage) *errorMessage = "IMAP FETCH 附件失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }
    // 响应定位：BODY[section] {K}\r\n<K字节>\r\n  （section 含 "." 时同样匹配）
    const QByteArray key = "BODY[" + section.toLatin1() + "]";
    for (int bp = rawStream.indexOf(key); bp >= 0;
         bp = rawStream.indexOf(key, bp + 1)) {
        const int ob = rawStream.indexOf('{', bp);
        const int cb = (ob >= 0) ? rawStream.indexOf('}', ob) : -1;
        if (cb <= ob) continue;
        bool ok = false;
        const int len = rawStream.mid(ob + 1, cb - ob - 1).toInt(&ok);
        int start = cb + 1;
        if (start < rawStream.size() && rawStream[start] == '\r') ++start;
        if (start < rawStream.size() && rawStream[start] == '\n') ++start;
        if (!ok || len < 0 || start + len > rawStream.size()) continue;
        QByteArray lit = rawStream.mid(start, len);
        // 按 encoding 解码到字节（实际的下载进度已由 sendCmd 接收阶段按传输字节上报）
        const QString e = encoding.trimmed().toLower();
        QByteArray out;
        constexpr int kProgressStep = 32 * 1024;
        if (e == "base64") {
            // base64：4 字符 → 3 字节；按输入分组解码并报告"已解码字节"
            const QByteArray clean = [&]{
                QByteArray c = lit;
                c.replace('\n', "").replace('\r', "").replace(' ', "").replace('\t', "");
                return c;
            }();
            // 解码阶段不再回调进度：网络接收阶段（sendCmd onLiteralProgress）
            // 已按传输字节平滑上报；解码只需整段一次（整段一次解码）
            QByteArray b64 = clean;
            int rem = b64.size() % 4;
            if (rem == 1) { b64.append("==="); }
            else if (rem == 2) { b64.append("=="); }
            else if (rem == 3) { b64.append("="); }
            out = QByteArray::fromBase64(b64);
        } else if (e == "quoted-printable") {
            QByteArray clean = lit;
            clean.replace("=\r\n", "").replace("=\n", "");
            // QP 不能按 32KB 切：=XX 跨片会被破坏（'=' 单独一片会被当成字面字符），
            // 必须整段一次解码；进度已由接收阶段上报
            out = qpDecodeBytes(clean);
        } else {
            // 7bit / 8bit / binary：原字节，直接按段拷贝（进度已由接收阶段上报）
            const int total = lit.size();
            out.reserve(total);
            for (int i = 0; i < total; i += kProgressStep) {
                int chunk = std::min(kProgressStep, total - i);
                out.append(lit.mid(i, chunk));
            }
        }
        if (onProgress) onProgress(out.size(), out.size());   // 100%
        if (decoded) *decoded = out;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return true;
    }
    // 诊断：把 rawStream 前 200 字节（不可打印字符替换为 ·）拼到错误信息，便于排障
    if (errorMessage) {
        QByteArray snip = rawStream.mid(0, 200);
        for (char& c : snip) {
            uchar uc = static_cast<uchar>(c);
            if (uc < 0x20 && c != '\r' && c != '\n' && c != '\t') c = '.';
            else if (uc == 0x7F) c = '.';
        }
        QString sectionTag = QStringLiteral("BODY[%1]").arg(section);
        bool hasBody = rawStream.contains(sectionTag.toUtf8());
        *errorMessage = QStringLiteral("IMAP FETCH 附件响应格式异常 (section=%1, 含BODY标签=%2, 原始前200字节=%3)")
            .arg(section)
            .arg(hasBody ? "Y" : "N")
            .arg(QString::fromUtf8(snip));
    }
    sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
    sock.disconnectFromHost();
    return false;
}

// ── 拉取整封原始 MIME 原文（用于"查看原件"） ──
//   命令：UID FETCH <uid> BODY.PEEK[]  →  返回整封 RFC822 原文（含全部 header/body）
//   响应： "* N FETCH (UID <uid> BODY[] {K}" + K 字节 literal + ")"
bool ImapClient::fetchRawSource(const Config& c, const QString& folder,
                                const QString& imapUid,
                                QByteArray* raw, QString* errorMessage) {
    if (raw) raw->clear();
    int timeoutMs = c.timeoutSec * 1000;
    QSslSocket sock;
    Tag tag;
    QStringList resp; QString status; QString err;
    if (!imapOpen(sock, c, folder, tag, &resp, &status, &err, errorMessage))
        return false;

    // BODY[] —— 整封邮件原文；BODY.PEEK 不标记已读
    if (!sendCmd(sock, tag,
                 QString("UID FETCH %1 BODY.PEEK[]").arg(imapUid),
                 timeoutMs, &resp, &status, &err)) {
        if (errorMessage) *errorMessage = "IMAP FETCH 原文失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }

    // 解析 literal 块大小
    for (int i = 0; i + 1 < resp.size(); ++i) {
        const QString& meta = resp[i];
        if (!meta.contains("BODY[]") && !meta.contains("BODY["))
            continue;
        if (!meta.endsWith('}')) continue;
        // meta: "* N FETCH (UID <uid> BODY[] {K}"
        int lb = meta.lastIndexOf('{');
        if (lb < 0) continue;
        int len = meta.mid(lb + 1, meta.size() - lb - 2).toInt();
        if (len <= 0 || len > 200 * 1024 * 1024) {  // 单封 200MB 上限
            if (errorMessage) *errorMessage = "IMAP FETCH 原文长度异常";
            continue;
        }
        // resp[i+1] 已被 readResponse 预读为 QString，但 literal 是 8-bit 字节
        // 我们从 socket 截取原始字节以保证附件二进制无损
        QString litLine = resp[i + 1];
        QByteArray bytes = litLine.toUtf8();
        // 若 pre-read 的字节不足（literal 被截断），尝试再读一次
        if (bytes.size() < len) {
            // 给一点时间等数据
            if (sock.waitForReadyRead(qMin(timeoutMs, 3000))) {
                bytes += sock.readAll();
            }
        }
        if (raw) *raw = bytes.left(len);
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return true;
    }
    if (errorMessage) *errorMessage = "IMAP FETCH 原文响应格式异常";
    sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
    sock.disconnectFromHost();
    return false;
}

bool ImapClient::markSeen(const Config& c, const QString& folder,
                          const QString& imapUid, QString* errorMessage) {
    return markSeenFlag(c, folder, imapUid, true, errorMessage);
}

bool ImapClient::markUnseen(const Config& c, const QString& folder,
                            const QString& imapUid, QString* errorMessage) {
    return markSeenFlag(c, folder, imapUid, false, errorMessage);
}

// 共用实现：seen=true → +FLAGS.SILENT (\Seen)（标已读）；false → -FLAGS.SILENT（标未读）
bool ImapClient::markSeenFlag(const Config& c, const QString& folder,
                              const QString& imapUid, bool seen,
                              QString* errorMessage) {
    int timeoutMs = c.timeoutSec * 1000;
    QSslSocket sock;
    Tag tag;
    QStringList resp; QString status; QString err;
    if (!imapOpen(sock, c, folder, tag, &resp, &status, &err, errorMessage))
        return false;
    const QString op = seen ? "+" : "-";
    bool ok = sendCmd(sock, tag,
                      QString("UID STORE %1 %2FLAGS.SILENT (\\Seen)").arg(imapUid).arg(op),
                      timeoutMs, &resp, &status, &err);
    if (!ok && errorMessage) *errorMessage = "IMAP STORE 失败: " + status;
    sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
    sock.disconnectFromHost();
    return ok;
}

bool ImapClient::markDeleted(const Config& c, const QString& folder,
                             const QString& imapUid, QString* errorMessage) {
    int timeoutMs = c.timeoutSec * 1000;
    QSslSocket sock;
    Tag tag;
    QStringList resp; QString status; QString err;
    if (!imapOpen(sock, c, folder, tag, &resp, &status, &err, errorMessage))
        return false;
    bool ok1 = sendCmd(sock, tag,
                       QString("UID STORE %1 +FLAGS.SILENT (\\Deleted)").arg(imapUid),
                       timeoutMs, &resp, &status, &err);
    if (!ok1) {
        if (errorMessage) *errorMessage = "IMAP STORE 失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }
    bool ok2 = sendCmd(sock, tag, "EXPUNGE", timeoutMs, &resp, &status, &err);
    if (!ok2 && errorMessage) *errorMessage = "IMAP EXPUNGE 失败: " + status;
    sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
    sock.disconnectFromHost();
    return ok2;
}

bool ImapClient::markDeletedBatch(const Config& c, const QString& folder,
                                  const QStringList& imapUids, QString* errorMessage) {
    if (imapUids.isEmpty()) return true;
    int timeoutMs = c.timeoutSec * 1000;
    QSslSocket sock;
    Tag tag;
    QStringList resp; QString status; QString err;
    if (!imapOpen(sock, c, folder, tag, &resp, &status, &err, errorMessage))
        return false;
    // uid-set 允许逗号分隔多 uid，一条 STORE 全部打 \Deleted
    bool ok1 = sendCmd(sock, tag,
                       QString("UID STORE %1 +FLAGS.SILENT (\\Deleted)").arg(imapUids.join(",")),
                       timeoutMs, &resp, &status, &err);
    if (!ok1) {
        if (errorMessage) *errorMessage = "IMAP STORE 失败: " + status;
        sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
        sock.disconnectFromHost();
        return false;
    }
    bool ok2 = sendCmd(sock, tag, "EXPUNGE", timeoutMs, &resp, &status, &err);
    if (!ok2 && errorMessage) *errorMessage = "IMAP EXPUNGE 失败: " + status;
    sendCmd(sock, tag, "LOGOUT", timeoutMs, &resp, &status, nullptr);
    sock.disconnectFromHost();
    return ok2;
}

// ── 本地附件提取（预览加速）────────────────────────────────
// 在已缓存的整封 RFC822 原文中，按 IMAP BODY[section]（与 collectAttachments
// 同一套"multipart 内局部计数"编号）递归定位并返回某 part 的原始 body 字节。
// 该封 rawSource 已在本地时，点附件可直接从原件切取，无需重新联网下载。
static QByteArray extractSectionFrom(const QString& headers, const QString& body,
                                     const QString& section) {
    QString ctype, cte, charset;
    readPartHeaders(headers, &ctype, &cte, &charset);
    if (ctype.contains("multipart/")) {
        QRegularExpression re("boundary=\"?([^\"\\s;]+)\"?",
                              QRegularExpression::CaseInsensitiveOption);
        auto m = re.match(headers);
        if (!m.hasMatch()) return {};
        const QString bnd = m.captured(1);
        QRegularExpression splitRe(QStringLiteral("\\r?\\n--") +
                                   QRegularExpression::escape(bnd));
        QString b2 = body;
        if (b2.startsWith(QStringLiteral("--") + bnd)) b2.prepend(QStringLiteral("\r\n"));
        else if (b2.startsWith(QStringLiteral("\n--") + bnd)) b2.prepend(QChar('\r'));
        const int target = section.section(QLatin1Char('.'), 0, 0).toInt();
        const QString rest = section.section(QLatin1Char('.'), 1);
        int sub = 0;
        for (const QString& p : b2.split(splitRe)) {
            if (p.startsWith(QStringLiteral("--"))) continue;
            int ps = p.indexOf(QStringLiteral("\r\n\r\n"));
            if (ps < 0) ps = p.indexOf(QStringLiteral("\n\n"));
            if (ps < 0) continue;
            ++sub;
            if (sub != target) continue;
            const QString partHeaders = p.left(ps);
            const QString partBody = p.mid(ps + (p.at(ps) == QLatin1Char('\r') ? 4 : 2));
            if (!rest.isEmpty())
                return extractSectionFrom(partHeaders, partBody, rest);
            return partBody.toLatin1();
        }
        return {};
    }
    // 非 multipart part：IMAP section "1" 即整段 body
    if (section == QLatin1String("1"))
        return body.toLatin1();
    return {};
}

bool ImapClient::extractAttachmentFromRaw(const QByteArray& rawSource,
                                          const QString& section,
                                          const QString& encoding,
                                          QByteArray* decoded) {
    if (decoded) decoded->clear();
    if (rawSource.isEmpty() || section.isEmpty()) return false;
    const QString text = QString::fromLatin1(rawSource);
    int sep = text.indexOf(QStringLiteral("\r\n\r\n"));
    if (sep < 0) sep = text.indexOf(QStringLiteral("\n\n"));
    if (sep < 0) return false;
    const QString topHeaders = text.left(sep);
    const QString topBody = text.mid(sep + (text.at(sep) == QLatin1Char('\r') ? 4 : 2));
    const QByteArray payload = extractSectionFrom(topHeaders, topBody, section);
    if (payload.isEmpty()) return false;

    const QString e = encoding.trimmed().toLower();
    QByteArray out;
    if (e == "base64") {
        QByteArray clean = payload;
        clean.replace('\n', "").replace('\r', "").replace(' ', "").replace('\t', "");
        out = QByteArray::fromBase64(clean);
    } else if (e == "quoted-printable") {
        QByteArray clean = payload;
        clean.replace("=\r\n", "").replace("=\n", "");
        out = qpDecodeBytes(clean);
    } else {
        out = payload;   // 7bit / 8bit / binary 原字节
    }
    if (decoded) *decoded = out;
    return !out.isEmpty();
}
