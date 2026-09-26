#include "mail/ScheduledQueue.h"

#include "mail/MailAccountManager.h"
#include "mail/SmtpClient.h"
#include "core/Logger.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>

ScheduledQueue& ScheduledQueue::instance() {
    static ScheduledQueue q;
    return q;
}

ScheduledQueue::ScheduledQueue() : QObject(nullptr) {
    loadFromDisk();
    // 每 30 秒扫描一次；近实时的精度对"定时发送"够用
    m_timer.setInterval(30 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &ScheduledQueue::onTimer);
    m_timer.start();
}

QString ScheduledQueue::dirPath() const {
    static QString p;
    if (p.isEmpty()) {
        p = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/mail/scheduled";
        QDir().mkpath(p);
    }
    return p;
}

QString ScheduledQueue::filePath(const QString& id) const {
    return dirPath() + "/" + id + ".json";
}

int ScheduledQueue::pendingCount() const {
    return m_items.size();
}

QString ScheduledQueue::schedule(Item it) {
    if (it.id.isEmpty()) it.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!it.fireAt.isValid()) it.fireAt = QDateTime::currentDateTime();
    // 落盘
    QJsonObject o;
    o["id"]           = it.id;
    o["fireAt"]       = it.fireAt.toString(Qt::ISODate);
    o["accountId"]    = it.accountId;
    o["to"]           = QJsonArray::fromStringList(it.to);
    o["cc"]           = QJsonArray::fromStringList(it.cc);
    o["bcc"]          = QJsonArray::fromStringList(it.bcc);
    o["subject"]      = it.subject;
    o["body"]         = it.body;
    o["htmlBody"]     = it.htmlBody;
    o["urgent"]       = it.urgent;
    o["readReceipt"]  = it.readReceipt;
    o["inReplyTo"]    = it.inReplyTo;
    o["references"]   = QJsonArray::fromStringList(it.references);
    QJsonArray arr;
    for (const auto& a : it.attachments) {
        QJsonObject aObj;
        aObj["filePath"]    = a.filePath;
        aObj["displayName"] = a.displayName;
        arr.append(aObj);
    }
    o["attachments"] = arr;
    QJsonArray iarr;
    for (const auto& im : it.inlineImages) {
        QJsonObject iObj;
        iObj["filePath"]    = im.filePath;
        iObj["displayName"] = im.displayName;
        iObj["contentId"]   = im.contentId;
        iarr.append(iObj);
    }
    o["inlineImages"] = iarr;

    QFile f(filePath(it.id));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::instance().error(
            QString("定时队列写盘失败: %1").arg(f.errorString()), "mail");
        return QString();
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();

    m_items.append(it);
    emit queueChanged();
    Logger::instance().info(
        QString("定时队列: 加入 id=%1 到点=%2 收件人=[%3] 主题=%4")
            .arg(it.id, it.fireAt.toString("yyyy-MM-dd HH:mm:ss"),
                 it.to.join(","), it.subject), "mail");
    return it.id;
}

bool ScheduledQueue::cancel(const QString& id) {
    // 从磁盘删
    QFile::remove(filePath(id));
    // 从内存删
    bool changed = false;
    for (int i = m_items.size() - 1; i >= 0; --i) {
        if (m_items[i].id == id) {
            m_items.removeAt(i);
            changed = true;
        }
    }
    if (changed) {
        emit queueChanged();
        Logger::instance().info(QString("定时队列: 取消 id=%1").arg(id), "mail");
    }
    return changed;
}

void ScheduledQueue::flushAll() {
    for (const auto& it : m_items) {
        schedule(it);   // 复用 schedule 写盘（id 已有所以不会重新生成）
    }
}

void ScheduledQueue::loadFromDisk() {
    m_items.clear();
    QDir d(dirPath());
    const QStringList files = d.entryList(QStringList{"*.json"}, QDir::Files);
    for (const QString& fn : files) {
        QFile f(d.absoluteFilePath(fn));
        if (!f.open(QIODevice::ReadOnly)) continue;
        QJsonParseError perr{};
        auto doc = QJsonDocument::fromJson(f.readAll(), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) continue;
        auto o = doc.object();
        Item it;
        it.id          = o["id"].toString();
        it.fireAt      = QDateTime::fromString(o["fireAt"].toString(), Qt::ISODate);
        it.accountId   = o["accountId"].toString();
        it.to          = o["to"].toVariant().toStringList();
        it.cc          = o["cc"].toVariant().toStringList();
        it.bcc         = o["bcc"].toVariant().toStringList();
        it.subject     = o["subject"].toString();
        it.body        = o["body"].toString();
        it.htmlBody    = o["htmlBody"].toString();
        it.urgent      = o["urgent"].toBool();
        it.readReceipt = o["readReceipt"].toBool();
        it.inReplyTo   = o["inReplyTo"].toString();
        it.references  = o["references"].toVariant().toStringList();
        for (const auto& v : o["attachments"].toArray()) {
            auto a = v.toObject();
            it.attachments.append({ a["filePath"].toString(), a["displayName"].toString() });
        }
        for (const auto& v : o["inlineImages"].toArray()) {
            auto a = v.toObject();
            it.inlineImages.append({ a["filePath"].toString(),
                                     a["displayName"].toString(),
                                     a["contentId"].toString() });
        }
        if (it.id.isEmpty() || !it.fireAt.isValid() || it.accountId.isEmpty()) {
            Logger::instance().warn(
                QString("定时队列: 跳过非法条目 %1").arg(fn), "mail");
            continue;
        }
        m_items.append(it);
    }
    if (!m_items.isEmpty()) {
        Logger::instance().info(
            QString("定时队列: 从磁盘恢复 %1 条待发邮件").arg(m_items.size()), "mail");
    }
}

void ScheduledQueue::onTimer() {
    if (m_items.isEmpty()) return;
    const QDateTime now = QDateTime::currentDateTime();
    for (int i = m_items.size() - 1; i >= 0; --i) {
        const Item it = m_items[i];
        if (it.fireAt > now) continue;
        if (m_inflight.contains(it.id)) continue;
        m_inflight.insert(it.id);
        fireOne(it);
    }
}

void ScheduledQueue::fireOne(const Item& it) {
    Logger::instance().info(
        QString("定时队列: 到点触发 id=%1 account=%2")
            .arg(it.id, it.accountId), "mail");

    auto* acc = MailAccountManager::instance().getById(it.accountId);
    if (!acc) {
        const QString err = QStringLiteral("账号不存在或已删除: %1").arg(it.accountId);
        Logger::instance().error(QString("定时队列失败: %1").arg(err), "mail");
        emit failed(it.id, err);
        m_inflight.remove(it.id);
        return;
    }

    SmtpClient::Params params;
    params.host      = acc->smtpHost;
    params.port      = acc->smtpPort;
    params.ssl       = acc->smtpSsl;
    params.username  = acc->email;
    params.password  = acc->password;
    params.fromName  = acc->displayName.isEmpty() ? acc->name : acc->displayName;
    params.fromEmail = acc->email;
    params.to        = it.to;
    params.cc        = it.cc;
    params.bcc       = it.bcc;
    params.subject   = it.subject;
    params.body      = it.body;
    params.htmlBody  = it.htmlBody;
    params.priority     = it.urgent ? 1 : 0;
    params.readReceipt  = it.readReceipt;
    params.inReplyTo    = it.inReplyTo;
    params.references   = it.references;
    for (const auto& a : it.attachments) {
        SmtpClient::Attachment att;
        att.filePath = a.filePath;
        att.fileName = a.displayName;
        params.attachments.append(att);
    }
    for (const auto& im : it.inlineImages) {
        SmtpClient::Attachment att;
        att.filePath = im.filePath;
        att.fileName = im.displayName;
        att.contentId = im.contentId;
        params.inlineImages.append(att);
    }

    QString err;
    bool ok = SmtpClient::send(params, &err);
    m_inflight.remove(it.id);

    if (ok) {
        Logger::instance().success(
            QString("定时队列: 已发送 id=%1 -> [%2]").arg(it.id, it.to.join(",")), "mail");
        // 成功后从队列 + 磁盘移除
        cancel(it.id);
        emit fired(it.id);
    } else {
        Logger::instance().error(
            QString("定时队列: 发送失败 id=%1 err=%2").arg(it.id, err), "mail");
        emit failed(it.id, err);
        // 失败保留文件，方便用户重试或取消
    }
}