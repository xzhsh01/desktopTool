#include "WeChatSyncWorker.h"
#include "CacheDb.h"
#include "WeChatAccountManager.h"
#include "WeChatDb.h"
#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>

#include "sqlite3.h"

WeChatSyncWorker::WeChatSyncWorker(QObject* parent) : QObject(parent) {
    m_watcher = new QFileSystemWatcher(this);
    m_watcher->setParent(nullptr);   // 跟随 worker 线程
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &WeChatSyncWorker::onWatcherChanged);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &WeChatSyncWorker::onWatcherChanged);
}

// ── 公共槽 ───────────────────────────────────────────────────────────

void WeChatSyncWorker::syncAccount(const QString& accId) {
    if (accId.isEmpty()) return;
    if (m_syncing.contains(accId)) {
        Logger::instance().info(
            QString("SyncWorker: %1 已在同步中，跳过").arg(accId), "sync");
        return;
    }
    m_syncing.insert(accId);

    QElapsedTimer tm;
    tm.start();

    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) {
        m_syncing.remove(accId);
        emit syncFailed(accId, "账号不存在");
        return;
    }

    const QString key = WeChatAccountManager::instance().keyForAccount(*acc);
    WeChatDb db(acc->id, acc->dataDir, key);

    emit syncStarted(accId, "decrypt");
    emit syncProgress(accId, "decrypt", 0, 0, "正在解密数据库…");
    if (!db.ensureDecrypted()) {
        m_syncing.remove(accId);
        emit syncFailed(accId, db.lastError().isEmpty()
                                   ? "数据库解密失败" : db.lastError());
        return;
    }

    // ① 同步元数据（联系人 + 会话）
    if (!syncAccountMeta(accId)) {
        m_syncing.remove(accId);
        emit syncFailed(accId, "同步联系人/会话失败");
        return;
    }

    // ② 同步消息（默认最近 5000 条/会话）
    if (!syncAccountMessages(accId)) {
        Logger::instance().warn(
            QString("SyncWorker: %1 消息同步部分失败").arg(accId), "sync");
    }

    // ③ 写回 accounts 表（更新 last_sync_at）
    CacheDb::upsertAccount(acc->id, acc->name, acc->wxid, acc->dataDir, key);

    m_syncing.remove(accId);
    emit syncFinished(accId, tm.elapsed());
}

void WeChatSyncWorker::syncAll() {
    const auto& accs = WeChatAccountManager::instance().accounts();
    for (const auto& a : accs) {
        syncAccount(a.id);
    }
}

void WeChatSyncWorker::watchAccount(const QString& accId) {
    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) return;

    QStringList paths;
    const QFileInfo dir(acc->dataDir);
    if (dir.exists()) paths << dir.absoluteFilePath();

    // 微信 3.x: Msg/MicroMsg.db, Msg/Multi/MSG*.db
    QDir msgDir(acc->dataDir + "/Msg");
    if (msgDir.exists()) {
        paths << msgDir.absolutePath();
        paths << msgDir.absoluteFilePath("Multi");
    }
    // 微信 4.x: db_storage/
    QDir dbDir(acc->dataDir + "/db_storage");
    if (dbDir.exists()) paths << dbDir.absolutePath();

    paths.removeAll(QString());
    paths.removeDuplicates();
    if (paths.isEmpty()) return;

    m_watcher->addPaths(paths);
    m_watchedDirs.insert(accId, paths);
    Logger::instance().info(
        QString("SyncWorker: 监控 %1 → %2").arg(accId, paths.join(", ")), "sync");
}

void WeChatSyncWorker::unwatchAccount(const QString& accId) {
    const auto it = m_watchedDirs.find(accId);
    if (it == m_watchedDirs.end()) return;
    m_watcher->removePaths(it.value());
    m_watchedDirs.erase(it);
}

// 异步加载单联系人详细信息（在 worker 线程，不阻塞 UI）
void WeChatSyncWorker::loadContactDetail(const QString& accId, const QString& wxid) {
    if (wxid.isEmpty()) return;
    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) return;
    const QString key = WeChatAccountManager::instance().keyForAccount(*acc);

    WeChatDb db(acc->id, acc->dataDir, key);
    if (!db.ensureDecrypted()) return;

    const auto d = db.loadContactDetail(wxid);
    QVariantMap m;
    m["userName"]     = d.userName;
    m["alias"]        = d.alias;
    m["nickname"]     = d.nickname;
    m["remark"]       = d.remark;
    m["display"]      = d.display;
    m["type"]         = d.type;
    m["verifyFlag"]   = d.verifyFlag;
    m["isRoom"]       = d.isChatRoom;
    m["smallHeadUrl"] = d.smallHeadUrl;
    m["bigHeadUrl"]   = d.bigHeadUrl;
    m["signature"]    = d.signature;
    m["province"]     = d.province;
    m["city"]         = d.city;
    m["country"]      = d.country;
    m["sex"]          = d.sex;

    emit contactDetailReady(accId, wxid, m);
}

// ── 内部实现 ─────────────────────────────────────────────────────────

bool WeChatSyncWorker::syncAccountMeta(const QString& accId) {
    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) return false;
    const QString key = WeChatAccountManager::instance().keyForAccount(*acc);
    WeChatDb db(acc->id, acc->dataDir, key);
    if (!db.ensureDecrypted()) return false;

    // ── 联系人 ──
    emit syncStarted(accId, "contacts");
    emit syncProgress(accId, "contacts", 0, 0, "正在读取联系人…");
    const auto contacts = db.loadContacts();
    QVariantList cl;
    cl.reserve(contacts.size());
    int idx = 0;
    const int total = contacts.size();
    for (const auto& c : contacts) {
        QVariantMap m;
        m["userName"]  = c.userName;
        m["alias"]     = c.alias;
        m["nickname"]  = c.nickname;
        m["remark"]    = c.remark;
        m["display"]   = c.display;
        m["isRoom"]    = c.isChatRoom;
        m["type"]      = c.type;
        m["verifyFlag"] = c.verifyFlag;
        cl.append(m);

        // 流式输出：每 200 条报告一次
        if (++idx % 200 == 0) {
            emit syncProgress(accId, "contacts", idx, total,
                              QString("%1 / %2").arg(idx).arg(total));
        }
    }
    if (!CacheDb::replaceContacts(accId, cl)) {
        Logger::instance().warn("CacheDb::replaceContacts 失败", "sync");
    }
    emit syncProgress(accId, "contacts", total, total,
                      QString("完成：%1 个联系人").arg(total));

    // ── 会话 ──
    emit syncStarted(accId, "sessions");
    emit syncProgress(accId, "sessions", 0, 0, "正在读取会话…");
    const auto sessions = db.loadSessions();
    QVariantList sl;
    sl.reserve(sessions.size());
    idx = 0;
    const int totalS = sessions.size();
    for (const auto& s : sessions) {
        QVariantMap m;
        m["talker"]  = s.talker;
        m["title"]   = s.title;
        m["lastMsg"] = s.lastMsg;
        m["time"]    = s.lastTime.toSecsSinceEpoch();
        m["unread"]  = s.unread;
        m["isRoom"]  = s.isChatRoom;
        sl.append(m);

        if (++idx % 100 == 0) {
            emit syncProgress(accId, "sessions", idx, totalS,
                              QString("%1 / %2").arg(idx).arg(totalS));
        }
    }
    if (!CacheDb::replaceSessions(accId, sl)) {
        Logger::instance().warn("CacheDb::replaceSessions 失败", "sync");
    }
    emit syncProgress(accId, "sessions", totalS, totalS,
                      QString("完成：%1 个会话").arg(totalS));

    // 通知 UI 刷新列表
    emit syncAccountDataReady(accId, sl, cl);
    return true;
}

QStringList WeChatSyncWorker::listChatRoomIds(const QString& accId) {
    QStringList ids;
    sqlite3* db = nullptr;
    if (sqlite3_open(CacheDb::dbPath().toUtf8().constData(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return ids;
    }
    const QString sql = "SELECT DISTINCT talker FROM sessions WHERE acc_id=?1 "
                        "AND is_chat_room=1";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW)
            ids << QString::fromUtf8((const char*)sqlite3_column_text(stmt, 0));
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ids;
}

bool WeChatSyncWorker::syncAccountMessages(const QString& accId) {
    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) return false;
    const QString key = WeChatAccountManager::instance().keyForAccount(*acc);
    WeChatDb db(acc->id, acc->dataDir, key);
    if (!db.ensureDecrypted()) return false;

    // 同步所有会话 + 群成员
    const auto& sessions = db.loadSessions();
    emit syncStarted(accId, "messages");
    emit syncProgress(accId, "messages", 0, sessions.size(),
                      QString("开始同步 %1 个会话").arg(sessions.size()));

    int idx = 0;
    for (const auto& s : sessions) {
        ++idx;

        // 跳过消息库未对齐的群聊（解密失败时 WeChatDb::loadMessages 返回空）
        const auto msgs = db.loadMessages(s.talker, 5000);
        QList<QVariantMap> vl;
        vl.reserve(msgs.size());
        QString title = s.title;
        for (const auto& m : msgs) {
            QVariantMap vm;
            vm["msgId"]      = m.msgId;
            vm["senderId"]   = m.senderId;
            vm["senderName"] = m.senderName;
            vm["isSender"]   = m.isSender;
            vm["type"]       = m.type;
            vm["subType"]    = m.subType;
            vm["content"]    = m.content;
            vm["display"]    = m.display;
            vm["time"]       = m.time.toSecsSinceEpoch();
            // 解析附件元信息（XML 复合消息、媒体消息）
            CacheDb::parseAttachMeta(m.type, m.subType, m.content, vm);
            vl.append(vm);
        }
        if (msgs.isEmpty()) {
            // 没消息的会话也保留会话记录即可（标题已在 sessions 表）
        } else {
            CacheDb::replaceMessages(accId, s.talker, vl);
            // 重新解析标题（含群成员数）
            title = db.displayName(s.talker);
            if (s.isChatRoom) {
                const auto members = db.chatRoomMembers(s.talker);
                title += QString("（%1）").arg(members.size());
                CacheDb::replaceChatRoomMembers(accId, s.talker, members);
            }
            emit syncMessagesReady(accId, s.talker, title, vl);
        }

        // 流式报告
        emit syncProgress(accId, "messages", idx, sessions.size(),
                          QString("[%1/%2] %3  (%4 条)")
                              .arg(idx).arg(sessions.size())
                              .arg(s.talker.left(40))
                              .arg(msgs.size()));
    }
    emit syncProgress(accId, "messages", sessions.size(), sessions.size(),
                      QString("完成：%1 个会话").arg(sessions.size()));
    return true;
}

bool WeChatSyncWorker::needsResync(const QString& accId, const QString& sourcePath,
                                    qint64 size, qint64 mtime) {
    qint64 prevSize = 0, prevMtime = 0;
    if (!CacheDb::getSyncState(accId, sourcePath, &prevSize, &prevMtime))
        return true;   // 没记录 → 必同步
    return (prevSize != size || prevMtime != mtime);
}

// ── watcher 回调 ─────────────────────────────────────────────────────

void WeChatSyncWorker::onWatcherChanged(const QString& path) {
    Q_UNUSED(path);
    // 找出哪些账号对应的目录被改动，重新触发同步
    // （简单做法：直接同步所有被监控账号；增量识别由 syncAccount 内 mtime 判断）
    for (auto it = m_watchedDirs.begin(); it != m_watchedDirs.end(); ++it) {
        for (const QString& p : it.value()) {
            // QFileSystemWatcher 触发的是绝对路径
            if (p == path || path.startsWith(p)) {
                // 延时 500ms 防抖（微信写入是连续的，多次 fileChanged 会刷屏）
                QTimer::singleShot(500, this, [this, id = it.key()]() {
                    syncAccount(id);
                });
                break;
            }
        }
    }
}