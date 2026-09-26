#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QDateTime>
#include <functional>
#include "mail/MailStore.h"

class QSslSocket;

/**
 * ImapClient: IMAP 收件客户端（同步、带超时）
 *
 * 仅实现最常用命令：LOGIN / LIST / SELECT / UID SEARCH / UID FETCH / LOGOUT
 * 用途：从收件箱拉取新邮件（用于提醒 / 离线浏览）
 *
 * SSL 直连（993 端口）；STARTTLS 可后续扩展
 */
class ImapClient {
public:
    struct Config {
        QString host;
        int     port = 993;
        bool    ssl  = true;
        QString username;
        QString password;
        int     timeoutSec = 30;
    };

    struct FetchedMessage {
        QString messageId;   // Message-ID header
        QString imapUid;     // IMAP UID（用于后续 FETCH BODY[TEXT] / STORE 等）
        QString from;
        QStringList to;
        QStringList cc;
        QString subject;
        QString body;        // 纯文本正文（多 part 取 text/plain）
        QDateTime date;
        bool seen = false;   // 服务器 FLAGS 含 \Seen（ENVELOPE 拉取时带回）
    };

    struct Folder {
        QString name;        // 原始名（可能含分隔符 / modified UTF-7 编码，SELECT 时用此名）
        QString delimiter;   // 层级分隔符（"/" 或 "."）
        QString flags;       // 如 "\HasNoChildren \Sent"（含 \Noselect 的不可选中）
    };

    // 解码 IMAP modified UTF-7 文件夹名（如 163/QQ 的中文文件夹）
    static QString decodeFolderName(const QString& imapName);

    // 拉取 INBOX 未读邮件列表
    // maxCount > 0: 限制最多拉取 maxCount 封最新(UID 最大)未读,避免 139 等
    //              服务器对单次 batch FETCH 大量 UID 限速导致超时
    static bool fetchUnread(const Config& cfg,
                            QList<FetchedMessage>* out,
                            QString* errorMessage,
                            int maxCount = 30);

    // 列出邮箱文件夹
    static bool listFolders(const Config& cfg,
                            QList<Folder>* out,
                            QString* errorMessage);

    // 仅获取邮件头部（用于快速预览列表，不下载正文）
    static bool fetchHeaders(const Config& cfg,
                             const QString& folder,
                             int maxCount,
                             QList<FetchedMessage>* out,
                             QString* errorMessage,
                             int* totalInFolder = nullptr);   // 服务器文件夹邮件总数（SEARCH ALL）

    // 带进度回调签名：每收到一段 literal 触发一次 (received, expected)。
    // 在调用方线程（后端 worker）同步触发，调用方需自行跨线程投递到 UI。
    using FetchProgressCb = std::function<void(qint64 received, qint64 expected)>;

    // 按 UID 拉取正文（text/plain 优先存 body；text/html 原样存 htmlBody 供富文本渲染）
    // rawSource 可选：整封原始 MIME 原文（内嵌图片解析等复用，免去二次 FETCH）
    // attsOut 可选：附件元数据列表（filename / mime / encoding / size / IMAP section）
    // onProgress 可选：literal 接收过程中回调 (received, expected)，触发节流由调用方控制
    static bool fetchBody(const Config& cfg,
                          const QString& folder,
                          const QString& imapUid,
                          QString* body,
                          QString* htmlBody,
                          QByteArray* rawSource,
                          QList<MailStore::Attachment>* attsOut = nullptr,
                          QString* errorMessage = nullptr,
                          const FetchProgressCb& onProgress = nullptr);

    // 预览加速快路径：仅拉正文（text/plain 或 text/html part），附件只从其
    // BODYSTRUCTURE 元数据生成（不下载附件内容），大幅减少带附件邮件的预览流量。
    // 仅适用于"纯 正文+普通附件"邮件；若含内嵌 cid 图 / message/rfc822 / signed 等
    // 复杂结构，或 BODYSTRUCTURE 解析失败，返回 false，调用方应回退 fetchBody 整封拉。
    // attsOut 可选：附件元数据（filename / mime / encoding / size / IMAP section）。
    static bool fetchBodyFast(const Config& cfg,
                              const QString& folder,
                              const QString& imapUid,
                              QString* body,
                              QString* htmlBody,
                              QList<MailStore::Attachment>* attsOut,
                              QString* errorMessage = nullptr,
                              const FetchProgressCb& onProgress = nullptr);

    // 按 UID 拉取整封原始 MIME 原文（含所有 header + multipart + 附件），
    // 用于"查看原件"功能以及附件解析
    static bool fetchRawSource(const Config& cfg,
                               const QString& folder,
                               const QString& imapUid,
                               QByteArray* raw,
                               QString* errorMessage);

    // 按 section 拉取单个 MIME part 并按其声明的 Content-Transfer-Encoding
    // 解码到 QByteArray（已解码字节，调用方可直接写文件）
    static bool fetchPart(const Config& cfg,
                          const QString& folder,
                          const QString& imapUid,
                          const QString& section,
                          const QString& encoding,
                          QByteArray* decoded,
                          QString* errorMessage);

    // 本地附件提取：从已缓存的整封 RFC822 原文(rawSource)中按 section 定位 part
    // 并按 encoding 解码到原始字节。预览已拉取过原件时，附件可直接本地切取而
    // 无需再次联网下载（提速核心）；section 编号与 collectAttachments 完全一致。
    static bool extractAttachmentFromRaw(const QByteArray& rawSource,
                                         const QString& section,
                                         const QString& encoding,
                                         QByteArray* decoded);

    // 带进度回调的 fetchPart（重载）：每收到 32KB 解码后字节触发一次 onProgress
    static bool fetchPart(const Config& cfg,
                          const QString& folder,
                          const QString& imapUid,
                          const QString& section,
                          const QString& encoding,
                          QByteArray* decoded,
                          QString* errorMessage,
                          const FetchProgressCb& onProgress);

    // 标记为已读：UID STORE <uid> +FLAGS.SILENT (\Seen)
    static bool markSeen(const Config& cfg,
                         const QString& folder,
                         const QString& imapUid,
                         QString* errorMessage);

    // 标记为未读：UID STORE <uid> -FLAGS.SILENT (\Seen)（把已读改回未读并同步服务器）
    static bool markUnseen(const Config& cfg,
                           const QString& folder,
                           const QString& imapUid,
                           QString* errorMessage);
    // 共用实现：seen=true → +FLAGS.SILENT \Seen；false → -FLAGS.SILENT \Seen
    static bool markSeenFlag(const Config& cfg, const QString& folder,
                             const QString& imapUid, bool seen,
                             QString* errorMessage);

    // 标记为删除：UID STORE <uid> +FLAGS.SILENT (\Deleted) + EXPUNGE
    static bool markDeleted(const Config& cfg,
                            const QString& folder,
                            const QString& imapUid,
                            QString* errorMessage);
    // 批量标记删除：同文件夹多封共用一次连接（UID STORE 多 uid + EXPUNGE），
    // 避免逐封建连（N 封 = N 次 LOGIN/SELECT/LOGOUT）拖慢批量删除
    static bool markDeletedBatch(const Config& cfg,
                                 const QString& folder,
                                 const QStringList& imapUids,
                                 QString* errorMessage);
};
