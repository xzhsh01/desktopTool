#include "mail/MailStore.h"
#include "core/Logger.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QRegularExpression>
#include <algorithm>

MailStore& MailStore::instance() {
    static MailStore s;
    return s;
}

MailStore::MailStore(QObject* parent) : QObject(parent) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/mail";
    QDir().mkpath(dir);
    m_draftsPath   = dir + "/drafts.json";
    m_messagesPath = dir + "/messages.json";
    loadDrafts();
    loadMessages();
    Logger::instance().info(
        QString("MailStore 初始化: drafts=%1, messages=%2, file=%3")
            .arg(m_drafts.size()).arg(m_messages.size()).arg(m_messagesPath),
        "mail");
}

// ─── 草稿 ────────────────────────────────────────────────────────────────────

void MailStore::loadDrafts() {
    m_drafts.clear();
    QFile f(m_draftsPath);
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return;
    for (const auto& v : doc.array()) {
        auto o = v.toObject();
        Draft d;
        d.id           = o["id"].toString();
        d.accountId    = o["accountId"].toString();
        d.to           = o["to"].toVariant().toStringList();
        d.cc           = o["cc"].toVariant().toStringList();
        d.bcc          = o["bcc"].toVariant().toStringList();
        d.subject      = o["subject"].toString();
        d.body         = o["body"].toString();
        d.htmlBody     = o["htmlBody"].toString();
        d.markdownMode = o["markdownMode"].toBool(false);
        d.attachmentPaths        = o["attachmentPaths"].toVariant().toStringList();
        d.attachmentDisplayNames = o["attachmentDisplayNames"].toVariant().toStringList();
        d.inReplyToId  = o["inReplyToId"].toString();
        d.forwardOfId  = o["forwardOfId"].toString();
        d.updatedAt    = QDateTime::fromString(o["updatedAt"].toString(), Qt::ISODate);
        m_drafts.append(d);
    }
}

void MailStore::saveDrafts() {
    QJsonArray arr;
    for (const auto& d : m_drafts) {
        QJsonObject o;
        o["id"]          = d.id;
        o["accountId"]   = d.accountId;
        o["to"]          = QJsonArray::fromStringList(d.to);
        o["cc"]          = QJsonArray::fromStringList(d.cc);
        o["bcc"]         = QJsonArray::fromStringList(d.bcc);
        o["subject"]     = d.subject;
        o["body"]        = d.body;
        o["htmlBody"]    = d.htmlBody;
        o["markdownMode"]= d.markdownMode;
        o["attachmentPaths"]        = QJsonArray::fromStringList(d.attachmentPaths);
        o["attachmentDisplayNames"] = QJsonArray::fromStringList(d.attachmentDisplayNames);
        o["inReplyToId"] = d.inReplyToId;
        o["forwardOfId"] = d.forwardOfId;
        o["updatedAt"]   = d.updatedAt.toString(Qt::ISODate);
        arr.append(o);
    }
    QFile f(m_draftsPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    }
}

MailStore::Draft* MailStore::draft(const QString& id) {
    for (auto& d : m_drafts) if (d.id == id) return &d;
    return nullptr;
}

MailStore::Draft MailStore::upsertDraft(const Draft& in) {
    Draft d = in;
    if (d.id.isEmpty()) d.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!d.updatedAt.isValid()) d.updatedAt = QDateTime::currentDateTime();
    if (auto* old = draft(d.id)) {
        *old = d;
    } else {
        m_drafts.append(d);
    }
    saveDrafts();
    emit draftsChanged();
    return d;
}

bool MailStore::removeDraft(const QString& id) {
    for (int i = 0; i < m_drafts.size(); ++i) {
        if (m_drafts[i].id == id) {
            m_drafts.removeAt(i);
            saveDrafts();
            emit draftsChanged();
            return true;
        }
    }
    return false;
}

// ─── 邮件缓存 ────────────────────────────────────────────────────────────────

void MailStore::loadMessages() {
    m_messages.clear();
    QFile f(m_messagesPath);
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return;
    for (const auto& v : doc.array()) {
        auto o = v.toObject();
        Message m;
        m.id         = o["id"].toString();
        m.accountId  = o["accountId"].toString();
        m.folder     = o["folder"].toString();
        m.messageId  = o["messageId"].toString();
        m.imapUid    = o["imapUid"].toString();
        m.from       = o["from"].toString();
        m.to         = o["to"].toVariant().toStringList();
        m.cc         = o["cc"].toVariant().toStringList();
        m.subject    = o["subject"].toString();
        m.body       = o["body"].toString();
        m.htmlBody   = o["htmlBody"].toString();
        m.date       = QDateTime::fromString(o["date"].toString(), Qt::ISODate);
        m.read       = o["read"].toBool(false);
        m.starred    = o["starred"].toBool(false);
        // 附件元数据（JSON 数组：{n,name,type,enc,size,sec}）
        const auto arr = o["attachments"].toArray();
        for (const auto& v : arr) {
            const auto ao = v.toObject();
            Attachment a;
            a.name     = ao["name"].toString();
            a.mimeType = ao["type"].toString();
            a.encoding = ao["enc"].toString();
            a.size     = ao["size"].toVariant().toLongLong();
            a.section  = ao["sec"].toString();
            if (!a.name.isEmpty() && !a.section.isEmpty())
                m.attachments.append(a);
        }
        m_messages.append(m);
    }
    // 历史数据自愈：空 id 补 UUID 并写回磁盘
    // （旧版本 upsert 去重覆盖曾把 id 清空，导致 selectedMessageId 永远为空、预览无法显示）
    bool fixed = false;
    for (auto& m : m_messages) {
        if (m.id.isEmpty()) {
            m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            fixed = true;
        }
    }
    if (fixed) {
        saveMessages();
        Logger::instance().info(
            QString("loadMessages: 空id自愈补齐并写回 (%1封)").arg(m_messages.size()), "mail");
    }
}

void MailStore::saveMessages() {
    QJsonArray arr;
    for (const auto& m : m_messages) {
        QJsonObject o;
        o["id"]        = m.id;
        o["accountId"] = m.accountId;
        o["folder"]    = m.folder;
        o["messageId"] = m.messageId;
        o["imapUid"]   = m.imapUid;
        o["from"]      = m.from;
        o["to"]        = QJsonArray::fromStringList(m.to);
        o["cc"]        = QJsonArray::fromStringList(m.cc);
        o["subject"]   = m.subject;
        o["body"]      = m.body;
        o["htmlBody"]  = m.htmlBody;
        o["date"]      = m.date.toString(Qt::ISODate);
        o["read"]      = m.read;
        o["starred"]   = m.starred;
        if (!m.attachments.isEmpty()) {
            QJsonArray attArr;
            for (const auto& a : m.attachments) {
                QJsonObject ao;
                ao["name"] = a.name;
                ao["type"] = a.mimeType;
                ao["enc"]  = a.encoding;
                ao["size"] = a.size;
                ao["sec"]  = a.section;
                attArr.append(ao);
            }
            o["attachments"] = attArr;
        }
        arr.append(o);
    }
    QFile f(m_messagesPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    }
}

QList<MailStore::Message> MailStore::messagesIn(const QString& accountId, const QString& folder) const {
    QList<Message> out;
    for (const auto& m : m_messages) {
        if (m.accountId == accountId && m.folder == folder) out.append(m);
    }
    std::sort(out.begin(), out.end(), [](const Message& a, const Message& b) {
        return a.date > b.date;
    });
    return out;
}

MailStore::Message* MailStore::message(const QString& id) {
    for (auto& m : m_messages) if (m.id == id) return &m;
    return nullptr;
}

int MailStore::unreadCount(const QString& accountId, const QString& folder) const {
    int n = 0;
    for (const auto& m : m_messages) {
        if (m.accountId == accountId && m.folder == folder && !m.read) ++n;
    }
    return n;
}

int MailStore::countIn(const QString& accountId, const QString& folder) const {
    int n = 0;
    for (const auto& m : m_messages) {
        if (m.accountId == accountId && m.folder == folder) ++n;
    }
    return n;
}

void MailStore::upsertMessages(const QList<Message>& list) {
    bool changed = false;
    int newCount = 0, dupCount = 0;
    QString lastAccountId;
    for (const auto& in : list) {
        // 去重键（按优先级）：
        // 1. RFC822 Message-ID（服务器 ENVELOPE 提供，真值）
        // 2. (accountId, imapUid)   —— 兜底：服务器 ENVELOPE 未返回 message-id 时用 IMAP UID
        // 3. id                      —— 兜底：本地新建邮件
        Message* target = nullptr;
        if (!in.messageId.isEmpty()) {
            for (auto& m : m_messages) {
                if (m.accountId == in.accountId && m.messageId == in.messageId) {
                    target = &m; break;
                }
            }
        }
        if (!target && !in.imapUid.isEmpty()) {
            for (auto& m : m_messages) {
                if (m.accountId == in.accountId && m.imapUid == in.imapUid) {
                    target = &m; break;
                }
            }
        }
        if (!target && !in.id.isEmpty()) {
            for (auto& m : m_messages) if (m.id == in.id) { target = &m; break; }
        }
        if (target) {
            // 覆盖前保留本地缓存的大字段与状态：
            // - body / htmlBody / rawSource：ENVELOPE 轮询不带正文，整结构覆盖
            //   会把懒拉取缓存的正文/原件清空 → 预览每分钟被清空重拉（回归防御）
            // - read：本地已读不因服务器 \Seen 缺失而降级
            const QString    keepId   = target->id;
            const QString    keepBody = target->body;
            const QString    keepHtml = target->htmlBody;
            const QByteArray keepRaw  = target->rawSource;
            const QList<Attachment> keepAtts = target->attachments;
            const bool       wasRead  = target->read;
            *target = in;
            // 去重覆盖时保留原 id：IMAP 拉取的 in.id 为空，
            // 直接 *target = in 会把已分配的稳定 id 清空（导致预览/标记已读失效）
            target->id = keepId;
            if (target->id.isEmpty())   // 历史数据 id 空 → 补生成
                target->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (target->body.isEmpty())
                target->body = keepBody;
            if (target->htmlBody.isEmpty())
                target->htmlBody = keepHtml;
            if (target->rawSource.isEmpty())
                target->rawSource = keepRaw;
            if (target->attachments.isEmpty())
                target->attachments = keepAtts;
            if (wasRead)
                target->read = true;
            ++dupCount;
            changed = true;
        } else {
            // 新增：分配 id（否则列表/预览/标记已读会失效）
            Message fresh = in;
            if (fresh.id.isEmpty()) {
                fresh.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            }
            m_messages.append(fresh);
            ++newCount;
            lastAccountId = fresh.accountId;
            changed = true;
        }
    }
    if (changed) {
        saveMessages();
        emit messagesChanged();
        if (newCount > 0 && !lastAccountId.isEmpty()) {
            emit newMailArrived(lastAccountId, unreadCount(lastAccountId, "INBOX"));
        }
    }
    Logger::instance().info(
        QString("MailStore.upsertMessages: in=%1 new=%2 dup=%3 total=%4")
            .arg(list.size()).arg(newCount).arg(dupCount).arg(m_messages.size()),
        "mail");
}

void MailStore::markRead(const QString& id, bool read) {
    if (auto* m = message(id)) {
        if (m->read != read) {
            m->read = read;
            saveMessages();
            emit messagesChanged();
        }
    }
}

bool MailStore::removeMessage(const QString& id) {
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].id == id) {
            m_messages.removeAt(i);
            saveMessages();
            emit messagesChanged();
            return true;
        }
    }
    return false;
}

int MailStore::removeMessagesByAccount(const QString& accountId) {
    int removed = 0;
    for (int i = m_messages.size() - 1; i >= 0; --i) {
        if (m_messages[i].accountId == accountId) {
            m_messages.removeAt(i);
            ++removed;
        }
    }
    if (removed > 0) {
        saveMessages();
        emit messagesChanged();
    }
    return removed;
}

int MailStore::removeOrphanMessages(const QStringList& validAccountIds) {
    QSet<QString> valid(validAccountIds.begin(), validAccountIds.end());
    int removed = 0;
    for (int i = m_messages.size() - 1; i >= 0; --i) {
        if (!valid.contains(m_messages[i].accountId)) {
            m_messages.removeAt(i);
            ++removed;
        }
    }
    if (removed > 0) {
        saveMessages();
        emit messagesChanged();
        Logger::instance().info(
            QString("MailStore 清理孤儿邮件: 移除 %1 封, 剩余 %2 封")
                .arg(removed).arg(m_messages.size()),
            "mail");
    }
    return removed;
}

bool MailStore::updateBody(const QString& id, const QString& body, const QString& htmlBody,
                           const QList<Attachment>& attachments) {
    if (auto* m = message(id)) {
        if (m->body != body || m->htmlBody != htmlBody) {
            m->body = body;
            m->htmlBody = htmlBody;
            m->attachments = attachments;
            saveMessages();
            emit messagesChanged();
        }
        // 细粒度信号：仅通知"这封邮件的正文变了"，让预览区刷新而不重建列表
        emit bodyUpdated(id);
        // rawSource 可能之前已写入（先 fetchRawSource 再 fetchBody）；检测 DSN 头
        if (!m_handledReadReceiptIds.contains(id) && !m->rawSource.isEmpty()) {
            QString rto = extractReadReceiptAddress(m->rawSource);
            if (!rto.isEmpty()) {
                m_handledReadReceiptIds.insert(id);
                emit readReceiptRequested(id, rto);
            }
        }
        return true;
    }
    return false;
}

bool MailStore::updateRawSource(const QString& id, const QByteArray& raw) {
    if (auto* m = message(id)) {
        // 大小保护:rawSource 可能异常大(恶意邮件或畸形 MIME),硬上限 5MB,
        // 超出截断并 warn,避免单封邮件占满内存或撑爆 messages.json
        constexpr qint64 RAW_SOURCE_MAX = 5 * 1024 * 1024;
        QByteArray clipped = raw;
        if (clipped.size() > RAW_SOURCE_MAX) {
            Logger::instance().warn(
                QString("rawSource 超大截断: id=%1 原 %2 字节 → 截到 %3 字节")
                    .arg(id).arg(raw.size()).arg(RAW_SOURCE_MAX),
                "mail");
            clipped.resize(RAW_SOURCE_MAX);
        }
        m->rawSource = clipped;
        // rawSource 可能很大(数 MB),改为非持久化字段,节省磁盘占用
        // 用细粒度信号而不是 messagesChanged,避免刷新整个邮件列表
        emit rawSourceUpdated(id);
        // raw 写入后检查 Disposition-Notification-To 头；只问一次
        if (!m_handledReadReceiptIds.contains(id)) {
            QString rto = extractReadReceiptAddress(m->rawSource);
            if (!rto.isEmpty()) {
                m_handledReadReceiptIds.insert(id);
                emit readReceiptRequested(id, rto);
            }
        }
        return true;
    }
    return false;
}

// 一次性写入 raw + 回填 messageId（ENVELOPE 漏给时从 raw header 解析）
bool MailStore::updateRawSource(const QString& id, const QByteArray& raw,
                                const QString& fillMessageId) {
    if (auto* m = message(id)) {
        if (m->messageId.isEmpty() && !fillMessageId.isEmpty()) {
            m->messageId = fillMessageId;
            // 持久化字段（messageId 落在 messages.json），写盘
            saveMessages();
        }
        return updateRawSource(id, raw);
    }
    return false;
}

// 强制重拉前的"清旧值"：把 body / htmlBody / rawSource / attachments 一次性清空，
// 写盘后发 bodyUpdated + rawSourceUpdated 让预览区回到"正在拉取正文…"占位。
// 已删除或本身就是空的也允许（幂等），返回 false 表示 id 不存在。
bool MailStore::clearBody(const QString& id) {
    auto* m = message(id);
    if (!m) return false;
    const bool anyChange = !m->body.isEmpty() || !m->htmlBody.isEmpty()
                        || !m->rawSource.isEmpty() || !m->attachments.isEmpty();
    m->body.clear();
    m->htmlBody.clear();
    m->rawSource.clear();
    m->attachments.clear();
    if (anyChange) saveMessages();   // 持久化字段（body/htmlBody/attachments）落盘
    // rawSource 不持久化，但仍发信号让监听者（"查看原件"按钮等）知道已清空
    emit rawSourceUpdated(id);
    emit bodyUpdated(id);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DSN (已读回执) 相关
// ─────────────────────────────────────────────────────────────────────────────

bool MailStore::hasHandledReadReceipt(const QString& id) const {
    return m_handledReadReceiptIds.contains(id);
}
void MailStore::markHandledReadReceipt(const QString& id) {
    m_handledReadReceiptIds.insert(id);
}

// 从 raw MIME 字节里抽出 Disposition-Notification-To 头值；找不到返回空。
// rawSource 是字节流，ASCII 头字段在 latin1/QString 视图下可直接匹配。
QString MailStore::extractReadReceiptAddress(const QByteArray& rawSource) {
    if (rawSource.isEmpty()) return QString();
    static const QRegularExpression re(
        QStringLiteral("^Disposition-Notification-To:[ \\t]*(.+(?:\\r?\\n[ \\t].+)*)"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    QString text = QString::fromLatin1(rawSource);
    auto m = re.match(text);
    if (!m.hasMatch()) return QString();
    QString value = m.captured(1).trimmed();
    if (value.isEmpty()) return QString();
    // 把 header value 里的折行（\r?\n[ \t]）先还原
    value.replace(QRegularExpression(QStringLiteral("\\r?\\n[ \\t]+")), QString());
    // 提取 <email>；没有尖括号则整体当作 email（简单兜底）
    QRegularExpression addrRe(QStringLiteral("<([^>]+)>"));
    auto am = addrRe.match(value);
    if (am.hasMatch()) return am.captured(1).trimmed();
    // 兜底：取首段空白之前
    int sp = value.indexOf(QRegularExpression(QStringLiteral("\\s")));
    return (sp < 0 ? value : value.left(sp)).trimmed();
}
