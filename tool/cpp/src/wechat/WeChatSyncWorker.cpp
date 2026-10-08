#include "WeChatSyncWorker.h"
#include "CacheDb.h"
#include "WeChatAccountManager.h"
#include "WeChatDb.h"
#include "core/Logger.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMap>
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

namespace {
// 计算列表整体指纹：每条目按 key 排序后拼接 → SHA1
// （与 ListPanel::fingerprint 同算法，UI 层与之保持一致）
QString listFingerprint(const QVariantList& list) {
    if (list.isEmpty()) return QStringLiteral("0");
    QCryptographicHash h(QCryptographicHash::Sha1);
    for (const auto& v : list) {
        const auto m = v.toMap();
        QMap<QString, QVariant> sorted(m);
        QString s;
        for (auto it = sorted.constBegin(); it != sorted.constEnd(); ++it) {
            s += it.key() + "=" + it.value().toString() + ";";
        }
        h.addData(s.toUtf8());
    }
    return QString::fromLatin1(h.result().toHex());
}
} // namespace

// ── 公共槽 ───────────────────────────────────────────────────────────

void WeChatSyncWorker::syncAccount(const QString& accId) {
    if (accId.isEmpty()) return;
    if (m_syncing.contains(accId)) {
        Logger::instance().info(
            QString("SyncWorker: %1 已在同步中，跳过").arg(accId), "sync");
        return;
    }
    m_syncing.insert(accId);
    m_pending.remove(accId);   // 已真正执行：清掉排队标记

    QElapsedTimer tm;
    tm.start();

    auto* acc = WeChatAccountManager::instance().getById(accId);
    if (!acc) {
        m_syncing.remove(accId);
        m_lastSyncAt.insert(accId, QDateTime::currentMSecsSinceEpoch());
        emit syncFailed(accId, "账号不存在");
        return;
    }

    const QString key = WeChatAccountManager::instance().keyForAccount(*acc);
    WeChatDb db(acc->id, acc->dataDir, key);

    emit syncStarted(accId, "decrypt");
    emit syncProgress(accId, "decrypt", 0, 0, "正在解密数据库…");
    if (!db.ensureDecrypted()) {
        m_syncing.remove(accId);
        m_lastSyncAt.insert(accId, QDateTime::currentMSecsSinceEpoch());
        emit syncFailed(accId, db.lastError().isEmpty()
                                   ? "数据库解密失败" : db.lastError());
        return;
    }

    // ① 同步元数据（联系人 + 会话）
    if (!syncAccountMeta(accId)) {
        m_syncing.remove(accId);
        m_lastSyncAt.insert(accId, QDateTime::currentMSecsSinceEpoch());
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
    // 记录同步完成时间，2 秒内不再响应 watcher（避免自触发环路）
    m_lastSyncAt.insert(accId, QDateTime::currentMSecsSinceEpoch());
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

    // 只监控具体 db 文件，不监控整个目录
    // （避免 path.startsWith(p) 把同一文件变化匹配到多层目录）
    auto addFileIfExists = [&](const QString& p) {
        if (QFile::exists(p)) paths << p;
    };

    // 微信 3.x
    addFileIfExists(acc->dataDir + "/Msg/MicroMsg.db");
    // Msg/Multi/MSG*.db 全部加入
    QDir multi3(acc->dataDir + "/Msg/Multi");
    if (multi3.exists()) {
        const auto msgs = multi3.entryList(QStringList() << "MSG*.db", QDir::Files);
        for (const auto& name : msgs)
            addFileIfExists(multi3.absoluteFilePath(name));
    }
    // 微信 4.x
    addFileIfExists(acc->dataDir + "/db_storage/contact/contact.db");
    addFileIfExists(acc->dataDir + "/db_storage/session/session.db");
    QDir msgDir4(acc->dataDir + "/db_storage/message");
    if (msgDir4.exists()) {
        const auto msgs = msgDir4.entryList(QStringList() << "message_*.db", QDir::Files);
        for (const auto& name : msgs)
            addFileIfExists(msgDir4.absoluteFilePath(name));
    }

    paths.removeAll(QString());
    paths.removeDuplicates();
    if (paths.isEmpty()) return;

    m_watcher->addPaths(paths);
    m_watchedDirs.insert(accId, paths);
    Logger::instance().info(
        QString("SyncWorker: 监控 %1 → %2 个文件").arg(accId).arg(paths.size()), "sync");
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

    // ── 联系人：先算出新内容 hash，与持久化 hash 对比 ──
    emit syncStarted(accId, "contacts");
    emit syncProgress(accId, "contacts", 0, 0, "正在读取联系人…");
    const auto contacts = db.loadContacts();
    QVariantList cl;
    cl.reserve(contacts.size());
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
    }
    const QString contactsNewFp = listFingerprint(cl);
    const QString contactsOldFp = CacheDb::getContentHash(accId, "contacts");
    const bool contactsChanged = (contactsNewFp != contactsOldFp);

    // ── 会话：先算出新内容 hash ──
    emit syncStarted(accId, "sessions");
    emit syncProgress(accId, "sessions", 0, 0, "正在读取会话…");
    const auto sessions = db.loadSessions();
    QVariantList sl;
    sl.reserve(sessions.size());
    for (const auto& s : sessions) {
        QVariantMap m;
        m["talker"]  = s.talker;
        m["title"]   = s.title;
        m["lastMsg"] = s.lastMsg;
        m["time"]    = s.lastTime.toSecsSinceEpoch();
        m["unread"]  = s.unread;
        m["isRoom"]  = s.isChatRoom;
        sl.append(m);
    }
    const QString sessionsNewFp = listFingerprint(sl);
    const QString sessionsOldFp = CacheDb::getContentHash(accId, "sessions");
    const bool sessionsChanged = (sessionsNewFp != sessionsOldFp);

    // ── 都没变：完全跳过 UI 信号（"没有新内容就不要刷新页面"） ──
    if (!contactsChanged && !sessionsChanged) {
        Logger::instance().debug(
            QString("SyncWorker: %1 元数据未变化，跳过所有 UI 信号").arg(accId), "sync");
        emit syncProgress(accId, "noop", 0, 0,
                          QString("内容未变化（%1 联系人 / %2 会话）")
                              .arg(cl.size()).arg(sl.size()));
        return true;
    }

    // ── 联系人变了：流式 emit + 写缓存 + 更新指纹 ──
    if (contactsChanged) {
        constexpr int kBatchSize = 200;
        QVariantList batch;
        batch.reserve(kBatchSize);
        int batchIdx = 0;
        const int total = cl.size();
        for (int i = 0; i < total; ++i) {
            batch.append(cl[i]);
            if ((i + 1) % kBatchSize == 0) {
                emit syncContactsPartial(accId, batch, batchIdx++);
                batch.clear();
                batch.reserve(kBatchSize);
                emit syncProgress(accId, "contacts", i + 1, total,
                                  QString("%1 / %2").arg(i + 1).arg(total));
            }
        }
        if (!batch.isEmpty()) emit syncContactsPartial(accId, batch, batchIdx);
        if (!CacheDb::replaceContacts(accId, cl)) {
            Logger::instance().warn("CacheDb::replaceContacts 失败", "sync");
        }
        CacheDb::setContentHash(accId, "contacts", contactsNewFp);
        emit syncProgress(accId, "contacts", total, total,
                          QString("完成：%1 个联系人").arg(total));
    } else {
        emit syncProgress(accId, "contacts", cl.size(), cl.size(),
                          QString("联系人未变化（%1 条）").arg(cl.size()));
    }

    // ── 会话变了：流式 emit + 写缓存 + 更新指纹 ──
    if (sessionsChanged) {
        constexpr int kBatchSizeS = 100;
        QVariantList batch;
        batch.reserve(kBatchSizeS);
        int batchIdx = 0;
        const int totalS = sl.size();
        for (int i = 0; i < totalS; ++i) {
            batch.append(sl[i]);
            if ((i + 1) % kBatchSizeS == 0) {
                emit syncSessionsPartial(accId, batch, batchIdx++);
                batch.clear();
                batch.reserve(kBatchSizeS);
                emit syncProgress(accId, "sessions", i + 1, totalS,
                                  QString("%1 / %2").arg(i + 1).arg(totalS));
            }
        }
        if (!batch.isEmpty()) emit syncSessionsPartial(accId, batch, batchIdx);
        if (!CacheDb::replaceSessions(accId, sl)) {
            Logger::instance().warn("CacheDb::replaceSessions 失败", "sync");
        }
        CacheDb::setContentHash(accId, "sessions", sessionsNewFp);
        emit syncProgress(accId, "sessions", totalS, totalS,
                          QString("完成：%1 个会话").arg(totalS));
    } else {
        emit syncProgress(accId, "sessions", sl.size(), sl.size(),
                          QString("会话未变化（%1 条）").arg(sl.size()));
    }

    // 至少一个变了：通知 UI 整体完成（让 listPanel 兜底重建，UI 层自身再 short-circuit）
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
            // 4.x loadMessages 已经从 packed_info_data 提取了图片 md5
            // 3.x 或 type=49 时 parseAttachMeta 会从 XML 中提取并覆盖
            if (!m.attachMd5.isEmpty()) vm["attachMd5"] = m.attachMd5;
            if (!m.attachAesKey.isEmpty()) vm["attachAesKey"] = m.attachAesKey;
            if (m.attachLength > 0)      vm["attachLength"] = m.attachLength;
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

void WeChatSyncWorker::scheduleSync(const QString& accId) {
    // 已经同步中 / 已排队 / 冷却期内 → 跳过
    if (m_syncing.contains(accId) || m_pending.contains(accId)) return;
    const qint64 lastMs = m_lastSyncAt.value(accId, 0);
    if (lastMs > 0 && QDateTime::currentMSecsSinceEpoch() - lastMs < 2000) {
        // 2 秒冷却：避免 watcher 触发解密 → 写缓存 → 触发更多 watcher → 反复同步
        Logger::instance().debug(
            QString("SyncWorker: %1 冷却期内，跳过").arg(accId), "sync");
        return;
    }

    m_pending.insert(accId);
    QTimer::singleShot(500, this, [this, id = accId]() {
        m_pending.remove(id);
        syncAccount(id);
    });
}

void WeChatSyncWorker::onWatcherChanged(const QString& path) {
    Q_UNUSED(path);
    // 仅判断具体 db 文件是否在被监控列表（精确匹配，不再 startsWith）
    for (auto it = m_watchedDirs.begin(); it != m_watchedDirs.end(); ++it) {
        const auto& paths = it.value();
        if (paths.contains(path)) {
            scheduleSync(it.key());
            break;   // 同一文件变化只算一次（不会重复匹配多层目录）
        }
    }
}