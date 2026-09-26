#pragma once

#include <QString>
#include <QByteArray>
#include <QList>
#include <functional>

#include "mail/MailStore.h"

/**
 * MailBodyFetcher: 正文拉取去重协调器
 *
 * 同一 (accountId, folder, imapUid) 在飞行中只发一次 IMAP 请求；
 * 期间再次请求（如预取进行中用户点击同一封）会合并到同一请求的回调列表。
 *
 * - 并发上限 3（139 等服务器对并发 FETCH 敏感），超出的自动排队
 * - IMAP IO 在后台线程；完成后的 store 写回与回调全部投递回主线程执行
 *   （MailStore 非线程安全，回调里操作 UI 也要求主线程）
 */
class MailBodyFetcher {
public:
    struct Result {
        bool        ok = false;
        QString     body;         // 纯文本正文
        QString     htmlBody;     // text/html 正文
        QByteArray  rawSource;    // 原始 MIME（可复用给"查看原件"）
        QList<MailStore::Attachment> attachments;
        QString     err;
    };
    using Callback = std::function<void(const Result&)>;

    // 异步发起；cb 保证在主线程执行。
    // 失败且无法发起（账户缺失等）时同步回调（调用栈仍有效，安全）。
    static void fetchAsync(const QString& accId,
                           const QString& folder,
                           const QString& imapUid,
                           const QString& msgId,
                           Callback cb);
};
