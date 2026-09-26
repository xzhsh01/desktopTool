#include "wechat/WeChatWorker.h"
#include "wechat/WeChatDb.h"
#include "wechat/WeChatAccountManager.h"
#include "wechat/WeChatKeyExtractor.h"
#include "core/Logger.h"

#include <QVariantMap>

WeChatWorker::WeChatWorker(QObject* parent) : QObject(parent) {}

// ─── 1. 加载账号的会话 + 联系人 ─────────────────────────────────────────────

void WeChatWorker::loadAccountData(const QString& accId) {
    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) {
        emit accountFailed(accId, QStringLiteral("账号不存在或已被删除"));
        return;
    }

    const QString key = WeChatAccountManager::instance().keyForAccount(*acc);
    WeChatDb db(acc->id, acc->dataDir, key);
    if (!db.ensureDecrypted()) {
        emit accountFailed(accId, QStringLiteral("数据库解密失败（请检查密钥/数据目录）"));
        return;
    }

    // 会话
    QVariantList sl;
    const auto sessions = db.loadSessions();
    sl.reserve(sessions.size());
    for (const auto& s : sessions) {
        QVariantMap vm;
        vm["talker"]  = s.talker;
        vm["title"]   = s.title;
        vm["lastMsg"] = s.lastMsg;
        vm["time"]    = s.lastTime;
        vm["unread"]  = s.unread;
        vm["isRoom"]  = s.isChatRoom;
        sl.append(vm);
    }

    // 联系人
    QVariantList cl;
    const auto contacts = db.loadContacts();
    cl.reserve(contacts.size());
    for (const auto& c : contacts) {
        QVariantMap vm;
        vm["userName"] = c.userName;
        vm["display"]  = c.display;
        vm["remark"]   = c.remark;
        vm["nickname"] = c.nickname;
        vm["alias"]    = c.alias;
        vm["isRoom"]   = c.isChatRoom;
        cl.append(vm);
    }

    Logger::instance().info(
        QString("WeChatWorker::loadAccountData ok: %1, %2 会话, %3 联系人")
            .arg(acc->name).arg(sl.size()).arg(cl.size()),
        "wechat.worker");

    emit accountLoaded(accId, sl, cl);
}

// ─── 2. 加载会话消息 ────────────────────────────────────────────────────────

void WeChatWorker::loadMessages(const QString& accId,
                                 const QString& talker,
                                 int limit) {
    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) {
        emit messagesFailed(accId, talker, QStringLiteral("账号不存在"));
        return;
    }

    const QString key = WeChatAccountManager::instance().keyForAccount(*acc);
    WeChatDb db(acc->id, acc->dataDir, key);
    if (!db.ensureDecrypted()) {
        emit messagesFailed(accId, talker, QStringLiteral("数据库解密失败"));
        return;
    }

    // 标题（先用缓存的会话标题；若空则从 db 解析）
    QString title;
    const auto msgs = db.loadMessages(talker, limit);
    title = db.displayName(talker);
    if (talker.endsWith(QLatin1String("@chatroom"))) {
        const QStringList members = db.chatRoomMembers(talker);
        title += QString("（%1）").arg(members.size());
    }

    QList<QVariantMap> vl;
    vl.reserve(msgs.size());
    for (const auto& m : msgs) {
        QVariantMap vm;
        vm["senderName"] = m.senderName;
        vm["senderId"]   = m.senderId;
        vm["isSender"]   = m.isSender;
        vm["type"]       = m.type;
        vm["subType"]    = m.subType;
        vm["content"]    = m.content;
        vm["display"]    = m.display;
        vm["time"]       = m.time;
        vl.append(vm);
    }

    Logger::instance().info(
        QString("WeChatWorker::loadMessages ok: %1/%2, %3 条")
            .arg(acc->name, talker).arg(vl.size()),
        "wechat.worker");

    emit messagesLoaded(accId, talker, title, vl);
}

// ─── 3. 提取密钥 ────────────────────────────────────────────────────────────

void WeChatWorker::extractKey(const QString& dbPath) {
    QString err;
    const QString key = WeChatKeyExtractor::extractFromRunningWeChat(dbPath, &err);
    if (key.isEmpty()) {
        Logger::instance().warn(
            QString("WeChatWorker::extractKey failed: %1").arg(err),
            "wechat.worker");
    } else {
        Logger::instance().info(
            "WeChatWorker::extractKey ok", "wechat.worker");
    }
    emit keyExtracted(key, err);
}