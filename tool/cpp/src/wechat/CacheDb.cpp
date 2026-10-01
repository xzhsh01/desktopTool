#include "CacheDb.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>

#include "sqlite3.h"

namespace {

// 全进程写互斥：避免 worker 线程与 UI 线程同时写库。
// 读不互斥（SQLite WAL 模式可读写并发）。
QMutex g_writeMutex;

bool openDb(sqlite3** out, QString* errOut) {
    if (sqlite3_open(CacheDb::dbPath().toUtf8().constData(), out) != SQLITE_OK) {
        if (errOut) *errOut = QString("打开缓存库失败: %1").arg(CacheDb::dbPath());
        return false;
    }
    sqlite3_exec(*out, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(*out, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(*out, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
    return true;
}

void closeDb(sqlite3* db) {
    if (db) sqlite3_close(db);
}

bool execSql(sqlite3* db, const QString& sql, QString* errOut = nullptr) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.toUtf8().constData(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (errOut) *errOut = QString::fromUtf8(err ? err : "");
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

}  // namespace

// ── 附件 XML 元信息提取 ────────────────────────────────────────────────────────

// 从 XML content 提取第一个 <tag>...</tag> 的内容（无 → 空串）
static QString xmlTag(const QString& xml, const QString& tag) {
    if (xml.isEmpty() || tag.isEmpty()) return {};
    const QString pat = "<" + tag + "[^>]*>";
    const QRegularExpression open(pat, QRegularExpression::CaseInsensitiveOption);
    const auto m1 = open.match(xml);
    if (!m1.hasMatch()) return {};
    const int start = m1.capturedEnd();
    const QRegularExpression close("</" + tag + ">", QRegularExpression::CaseInsensitiveOption);
    const auto m2 = close.match(xml, start);
    if (!m2.hasMatch()) return {};
    QString out = xml.mid(start, m2.capturedStart() - start);
    return out.simplified();
}

// 从 XML attribute 提取单值（无 → 空串）
static QString xmlAttr(const QString& xml, const QString& attr) {
    if (xml.isEmpty() || attr.isEmpty()) return {};
    static thread_local QRegularExpression re(QStringLiteral("([A-Za-z0-9_:]+)"),
                                              QRegularExpression::CaseInsensitiveOption);
    Q_UNUSED(re);
    const QString needle = attr + "=\"";
    const int p1 = xml.indexOf(needle, 0, Qt::CaseInsensitive);
    if (p1 < 0) return {};
    const int start = p1 + needle.size();
    const int end = xml.indexOf('"', start);
    if (end < 0) return {};
    return xml.mid(start, end - start);
}

// 从 type 3 / 47 的 StrContent XML 提取图片 md5
// 优先取原图 md5，否则中图 midimgmd5 / cdnmidimgmd5，否则缩略图 cdnthumbmd5。
QString CacheDb::extractImageMd5FromXml(const QString& xml) {
    if (xml.isEmpty()) return {};
    static const char* attrs[] = {"md5", "midimgmd5", "cdnmidimgmd5", "cdnthumbmd5", nullptr};
    for (int i = 0; attrs[i]; ++i) {
        const QString v = xmlAttr(xml, QString::fromLatin1(attrs[i]));
        if (v.size() == 32 && v.contains(QRegularExpression(QStringLiteral("^[0-9a-fA-F]{32}$"))))
            return v.toLower();
    }
    return {};
}

// 从 type 34 / 43 的 StrContent XML 提取附件 md5（<voicemsg>/<videomsg> 等）
QString CacheDb::extractFileMd5FromXml(const QString& xml) {
    if (xml.isEmpty()) return {};
    const QString v = xmlAttr(xml, QStringLiteral("md5"));
    if (v.size() == 32 && v.contains(QRegularExpression(QStringLiteral("^[0-9a-fA-F]{32}$"))))
        return v.toLower();
    return {};
}

void CacheDb::parseAttachMeta(int type, int subType,
                              const QString& content, QVariantMap& m) {
    // 只对已知有附件结构的类型解析；其它清空
    m["attachTitle"] = QString();
    m["attachSize"]  = qint64(0);
    m["attachExt"]   = QString();
    m["attachUrl"]   = QString();
    m["attachMime"]  = QString();
    m["attachMd5"]   = QString();
    if (content.isEmpty()) return;

    auto pickNum = [](const QString& s) -> qint64 {
        if (s.isEmpty()) return 0;
        bool ok = false;
        const qint64 v = s.toLongLong(&ok);
        return ok ? v : 0;
    };

    // type 49 = XML 复合消息
    if (type == 49) {
        const QString appmsg = xmlTag(content, "appmsg");
        if (appmsg.isEmpty()) return;

        const QString title = xmlTag(appmsg, "title");
        const QString url   = xmlTag(appmsg, "url");
        const QString mime  = xmlTag(appmsg, "type");  // appmsg/<type> 是消息子类型文本
        QString ext, sizeStr;

        switch (subType) {
        case 4: {  // 文件
            const QString fileInfoTag = xmlTag(appmsg, "appattach");
            ext     = xmlTag(fileInfoTag, "fileext").toLower();
            sizeStr = xmlTag(fileInfoTag, "totallen");
            break;
        }
        case 5:   // 链接
        case 88:  // 公众号文章
        case 19:  // 聊天记录
            break;
        case 6: { // 音乐
            ext = "mp3";
            break;
        }
        case 33:  // 小程序
        case 36: {
            const QString weapp = xmlTag(appmsg, "weappinfo");
            ext = xmlTag(weapp, "appid");
            break;
        }
        case 57: { // 引用消息
            const QString refer = xmlTag(appmsg, "refermsg");
            const QString rt = xmlTag(refer, "content");
            if (!rt.isEmpty()) m["attachTitle"] = QString("引用：") + rt;
            m["attachMime"] = "quote";
            return;
        }
        case 2000: { // 转账
            const QString pay = xmlTag(appmsg, "pay_memo");
            if (!pay.isEmpty()) m["attachTitle"] = pay;
            m["attachMime"] = "transfer";
            return;
        }
        default:
            break;
        }

        m["attachTitle"] = title;
        m["attachSize"]  = pickNum(sizeStr);
        m["attachExt"]   = ext;
        m["attachUrl"]   = url;
        m["attachMime"]  = mime;
        return;
    }

    // 非 XML 类型，但有明确语义 → 给个扩展名（便于 UI 显示）
    switch (type) {
    case 3:
        m["attachExt"]  = "image";
        m["attachMime"] = "image";
        m["attachMd5"]  = extractImageMd5FromXml(content);
        break;
    case 34:
        m["attachExt"]  = "voice";
        m["attachMime"] = "voice";
        m["attachMd5"]  = extractFileMd5FromXml(content);
        break;
    case 43:
        m["attachExt"]  = "video";
        m["attachMime"] = "video";
        m["attachMd5"]  = extractFileMd5FromXml(content);
        break;
    case 47:
        m["attachExt"]  = "gif";
        m["attachMime"] = "gif";
        m["attachMd5"]  = extractImageMd5FromXml(content);
        break;
    case 48:
        m["attachExt"]  = "loc";
        m["attachMime"] = "location";
        break;
    default: break;
    }
}

QString CacheDb::dbPath() {
    static QString path;
    if (path.isEmpty()) {
        const QString base =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(base);
        path = base + "/cache.sqlite";
    }
    return path;
}

bool CacheDb::initialize(QString* errOut) {
    sqlite3* db = nullptr;
    if (!openDb(&db, errOut)) return false;

    const QString ddl = R"SQL(
        CREATE TABLE IF NOT EXISTS accounts (
            acc_id    TEXT PRIMARY KEY,
            name      TEXT,
            wxid      TEXT,
            data_dir  TEXT,
            key_hex   TEXT,
            updated_at INTEGER
        );

        CREATE TABLE IF NOT EXISTS sessions (
            acc_id     TEXT,
            talker     TEXT,
            title      TEXT,
            last_msg   TEXT,
            last_time  INTEGER,
            unread     INTEGER,
            is_chat_room INTEGER,
            PRIMARY KEY (acc_id, talker)
        );
        CREATE INDEX IF NOT EXISTS idx_sessions_acc_time
            ON sessions(acc_id, last_time DESC);

        CREATE TABLE IF NOT EXISTS contacts (
            acc_id     TEXT,
            user_name  TEXT,
            alias      TEXT,
            nickname   TEXT,
            remark     TEXT,
            display    TEXT,
            is_chat_room INTEGER,
            type       INTEGER,
            verify_flag INTEGER,
            PRIMARY KEY (acc_id, user_name)
        );
        CREATE INDEX IF NOT EXISTS idx_contacts_acc_display
            ON contacts(acc_id, display);

        CREATE TABLE IF NOT EXISTS messages (
            acc_id     TEXT,
            talker     TEXT,
            msg_id     INTEGER,
            sender_id  TEXT,
            sender_name TEXT,
            is_sender  INTEGER,
            type       INTEGER,
            sub_type   INTEGER,
            content    TEXT,
            display    TEXT,
            time       INTEGER,
            attach_title TEXT,
            attach_size  INTEGER,
            attach_ext   TEXT,
            attach_url   TEXT,
            attach_mime  TEXT,
            attach_md5   TEXT,
            PRIMARY KEY (acc_id, msg_id)
        );
        CREATE INDEX IF NOT EXISTS idx_messages_acc_talker_time
            ON messages(acc_id, talker, time);

        CREATE TABLE IF NOT EXISTS chat_room_members (
            acc_id     TEXT,
            chat_room_id TEXT,
            wxid       TEXT,
            PRIMARY KEY (acc_id, chat_room_id, wxid)
        );

        CREATE TABLE IF NOT EXISTS sync_state (
            acc_id     TEXT,
            source_path TEXT,
            source_size INTEGER,
            source_mtime INTEGER,
            synced_at  INTEGER,
            PRIMARY KEY (acc_id, source_path)
        );

        -- 内容指纹：worker 写入 sessions/contacts 后写入；
        -- 下次同步前对比，未变则跳过 UI 信号（"没有新内容就不要刷新页面"）
        CREATE TABLE IF NOT EXISTS sync_content_hash (
            acc_id     TEXT,
            kind       TEXT,    -- "sessions" / "contacts"
            hash       TEXT,
            synced_at  INTEGER,
            PRIMARY KEY (acc_id, kind)
        );
    )SQL";

    bool ok = execSql(db, ddl, errOut);

    // 兼容老库：单独再开一次连接做 ALTER（SQLite 不能在创建表的同一事务里 ALTER）
    if (ok) {
        sqlite3* db2 = nullptr;
        if (openDb(&db2, nullptr)) {
            auto hasCol = [&](const char* col) {
                sqlite3_stmt* st = nullptr;
                const QString sql = "SELECT 1 FROM pragma_table_info('messages') WHERE name=?1";
                bool exists = false;
                if (sqlite3_prepare_v2(db2, sql.toUtf8().constData(), -1, &st, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, col, -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(st) == SQLITE_ROW) exists = true;
                }
                if (st) sqlite3_finalize(st);
                return exists;
            };
            if (!hasCol("attach_title")) execSql(db2, "ALTER TABLE messages ADD COLUMN attach_title TEXT", nullptr);
            if (!hasCol("attach_size"))  execSql(db2, "ALTER TABLE messages ADD COLUMN attach_size  INTEGER", nullptr);
            if (!hasCol("attach_ext"))   execSql(db2, "ALTER TABLE messages ADD COLUMN attach_ext   TEXT", nullptr);
            if (!hasCol("attach_url"))   execSql(db2, "ALTER TABLE messages ADD COLUMN attach_url   TEXT", nullptr);
            if (!hasCol("attach_mime"))  execSql(db2, "ALTER TABLE messages ADD COLUMN attach_mime  TEXT", nullptr);
            if (!hasCol("attach_md5"))   execSql(db2, "ALTER TABLE messages ADD COLUMN attach_md5   TEXT", nullptr);
            closeDb(db2);
        }
    }

    closeDb(db);

    if (ok) {
        Logger::instance().info(
            QString("CacheDb 已就绪: %1").arg(dbPath()), "cache");
    } else {
        Logger::instance().error(
            QString("CacheDb 初始化失败: %1").arg(errOut ? *errOut : ""), "cache");
    }
    return ok;
}

// ── 账号 ────────────────────────────────────────────────────────────

bool CacheDb::upsertAccount(const QString& accId, const QString& name,
                            const QString& wxid, const QString& dataDir,
                            const QString& keyHex) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    const QString sql = R"SQL(
        INSERT INTO accounts (acc_id, name, wxid, data_dir, key_hex, updated_at)
        VALUES (?1, ?2, ?3, ?4, ?5, strftime('%s','now'))
        ON CONFLICT(acc_id) DO UPDATE SET
            name=excluded.name, wxid=excluded.wxid, data_dir=excluded.data_dir,
            key_hex=excluded.key_hex, updated_at=excluded.updated_at
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    bool ok = (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK);
    if (ok) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name.toUtf8().constData(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, wxid.toUtf8().constData(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, dataDir.toUtf8().constData(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, keyHex.toUtf8().constData(),    -1, SQLITE_TRANSIENT);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return ok;
}

QList<QVariantMap> CacheDb::loadAccounts() {
    QList<QVariantMap> out;
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return out;
    const QString sql = "SELECT acc_id, name, wxid, data_dir, updated_at FROM accounts";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            QVariantMap m;
            m["accId"]     = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 0));
            m["name"]      = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 1));
            m["wxid"]      = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 2));
            m["dataDir"]   = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 3));
            m["updatedAt"] = qint64(sqlite3_column_int64(stmt, 4));
            out.append(m);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return out;
}

bool CacheDb::deleteAccount(const QString& accId) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    bool ok = true;
    const QStringList sqls = {
        "DELETE FROM sessions WHERE acc_id=?1",
        "DELETE FROM contacts WHERE acc_id=?1",
        "DELETE FROM messages WHERE acc_id=?1",
        "DELETE FROM chat_room_members WHERE acc_id=?1",
        "DELETE FROM sync_state WHERE acc_id=?1",
        "DELETE FROM sync_content_hash WHERE acc_id=?1",
        "DELETE FROM accounts WHERE acc_id=?1",
    };
    for (const QString& sql : sqls) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
        }
        if (stmt) sqlite3_finalize(stmt);
    }
    closeDb(db);
    return ok;
}

// ── 会话 ────────────────────────────────────────────────────────────

bool replaceSessionsInternal(sqlite3* db, const QString& accId, const QVariantList& rows) {
    execSql(db, "BEGIN", nullptr);
    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE acc_id=?1", -1, &del, nullptr);
    sqlite3_bind_text(del, 1, accId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_finalize(del);

    const QString sql = R"SQL(
        INSERT INTO sessions (acc_id, talker, title, last_msg, last_time, unread, is_chat_room)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
    )SQL";
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &ins, nullptr);
    for (const auto& v : rows) {
        const auto m = v.toMap();
        sqlite3_reset(ins);
        sqlite3_bind_text  (ins, 1, accId.toUtf8().constData(),            -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins, 2, m["talker"].toString().toUtf8().constData(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins, 3, m["title"].toString().toUtf8().constData(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins, 4, m["lastMsg"].toString().toUtf8().constData(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (ins, 5, m["time"].toLongLong());
        sqlite3_bind_int   (ins, 6, m["unread"].toInt());
        sqlite3_bind_int   (ins, 7, m["isRoom"].toBool() ? 1 : 0);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);
    return execSql(db, "COMMIT", nullptr);
}

bool CacheDb::replaceSessions(const QString& accId, const QVariantList& rows) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    const bool ok = replaceSessionsInternal(db, accId, rows);
    closeDb(db);
    return ok;
}

bool upsertSessionsInternal(sqlite3* db, const QString& accId, const QVariantList& rows) {
    const QString sql = R"SQL(
        INSERT INTO sessions (acc_id, talker, title, last_msg, last_time, unread, is_chat_room)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
        ON CONFLICT(acc_id, talker) DO UPDATE SET
            title=excluded.title, last_msg=excluded.last_msg,
            last_time=excluded.last_time, unread=excluded.unread,
            is_chat_room=excluded.is_chat_room
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    for (const auto& v : rows) {
        const auto m = v.toMap();
        sqlite3_reset(stmt);
        sqlite3_bind_text  (stmt, 1, accId.toUtf8().constData(),            -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, m["talker"].toString().toUtf8().constData(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 3, m["title"].toString().toUtf8().constData(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 4, m["lastMsg"].toString().toUtf8().constData(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 5, m["time"].toLongLong());
        sqlite3_bind_int   (stmt, 6, m["unread"].toInt());
        sqlite3_bind_int   (stmt, 7, m["isRoom"].toBool() ? 1 : 0);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool CacheDb::upsertSessions(const QString& accId, const QVariantList& rows) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    const bool ok = upsertSessionsInternal(db, accId, rows);
    closeDb(db);
    return ok;
}

QVariantList CacheDb::loadSessions(const QString& accId, int limit) {
    QVariantList out;
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return out;
    QString sql =
        "SELECT talker, title, last_msg, last_time, unread, is_chat_room "
        "FROM sessions WHERE acc_id=?1 ORDER BY last_time DESC";
    if (limit > 0) sql += QString(" LIMIT %1").arg(limit);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            QVariantMap m;
            m["talker"]    = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 0));
            m["title"]     = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 1));
            m["lastMsg"]   = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 2));
            m["time"]      = qint64(sqlite3_column_int64(stmt, 3));
            m["unread"]    = sqlite3_column_int(stmt, 4);
            m["isRoom"]    = sqlite3_column_int(stmt, 5) != 0;
            out.append(m);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return out;
}

// ── 联系人 ──────────────────────────────────────────────────────────

bool replaceContactsInternal(sqlite3* db, const QString& accId, const QVariantList& rows) {
    execSql(db, "BEGIN", nullptr);
    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM contacts WHERE acc_id=?1", -1, &del, nullptr);
    sqlite3_bind_text(del, 1, accId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_finalize(del);

    const QString sql = R"SQL(
        INSERT INTO contacts (acc_id, user_name, alias, nickname, remark, display,
                              is_chat_room, type, verify_flag)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
    )SQL";
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &ins, nullptr);
    for (const auto& v : rows) {
        const auto m = v.toMap();
        sqlite3_reset(ins);
        sqlite3_bind_text  (ins, 1, accId.toUtf8().constData(),              -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins, 2, m["userName"].toString().toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins, 3, m["alias"].toString().toUtf8().constData(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins, 4, m["nickname"].toString().toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins, 5, m["remark"].toString().toUtf8().constData(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins, 6, m["display"].toString().toUtf8().constData(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (ins, 7, m["isRoom"].toBool() ? 1 : 0);
        sqlite3_bind_int   (ins, 8, m["type"].toInt());
        sqlite3_bind_int   (ins, 9, m["verifyFlag"].toInt());
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);
    return execSql(db, "COMMIT", nullptr);
}

bool CacheDb::replaceContacts(const QString& accId, const QVariantList& rows) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    const bool ok = replaceContactsInternal(db, accId, rows);
    closeDb(db);
    return ok;
}

bool upsertContactsInternal(sqlite3* db, const QString& accId, const QVariantList& rows) {
    const QString sql = R"SQL(
        INSERT INTO contacts (acc_id, user_name, alias, nickname, remark, display,
                              is_chat_room, type, verify_flag)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
        ON CONFLICT(acc_id, user_name) DO UPDATE SET
            alias=excluded.alias, nickname=excluded.nickname,
            remark=excluded.remark, display=excluded.display,
            is_chat_room=excluded.is_chat_room, type=excluded.type
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    for (const auto& v : rows) {
        const auto m = v.toMap();
        sqlite3_reset(stmt);
        sqlite3_bind_text  (stmt, 1, accId.toUtf8().constData(),              -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, m["userName"].toString().toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 3, m["alias"].toString().toUtf8().constData(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 4, m["nickname"].toString().toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 5, m["remark"].toString().toUtf8().constData(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 6, m["display"].toString().toUtf8().constData(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 7, m["isRoom"].toBool() ? 1 : 0);
        sqlite3_bind_int   (stmt, 8, m["type"].toInt());
        sqlite3_bind_int   (stmt, 9, m["verifyFlag"].toInt());
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool CacheDb::upsertContacts(const QString& accId, const QVariantList& rows) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    const bool ok = upsertContactsInternal(db, accId, rows);
    closeDb(db);
    return ok;
}

QVariantList CacheDb::loadContacts(const QString& accId) {
    QVariantList out;
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return out;
    const QString sql =
        "SELECT user_name, alias, nickname, remark, display, is_chat_room, type, verify_flag "
        "FROM contacts WHERE acc_id=?1 ORDER BY display";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            QVariantMap m;
            m["userName"]  = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 0));
            m["alias"]     = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 1));
            m["nickname"]  = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 2));
            m["remark"]    = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 3));
            m["display"]   = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 4));
            m["isRoom"]    = sqlite3_column_int(stmt, 5) != 0;
            m["type"]      = sqlite3_column_int(stmt, 6);
            m["verifyFlag"] = sqlite3_column_int(stmt, 7);
            out.append(m);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return out;
}

// ── 消息 ────────────────────────────────────────────────────────────

bool replaceMessagesInternal(sqlite3* db, const QString& accId, const QString& talker,
                              const QList<QVariantMap>& rows) {
    execSql(db, "BEGIN", nullptr);
    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM messages WHERE acc_id=?1 AND talker=?2",
                       -1, &del, nullptr);
    sqlite3_bind_text(del, 1, accId.toUtf8().constData(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(del, 2, talker.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_finalize(del);

    const QString sql = R"SQL(
        INSERT INTO messages (acc_id, talker, msg_id, sender_id, sender_name, is_sender,
                              type, sub_type, content, display, time,
                              attach_title, attach_size, attach_ext, attach_url, attach_mime,
                              attach_md5)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17)
    )SQL";
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &ins, nullptr);
    for (const auto& m : rows) {
        sqlite3_reset(ins);
        sqlite3_bind_text  (ins, 1, accId.toUtf8().constData(),                -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins, 2, talker.toUtf8().constData(),               -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (ins, 3, m["msgId"].toLongLong());
        sqlite3_bind_text  (ins, 4, m["senderId"].toString().toUtf8().constData(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins, 5, m["senderName"].toString().toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (ins, 6, m["isSender"].toBool() ? 1 : 0);
        sqlite3_bind_int   (ins, 7, m["type"].toInt());
        sqlite3_bind_int   (ins, 8, m["subType"].toInt());
        sqlite3_bind_text  (ins, 9, m["content"].toString().toUtf8().constData(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins,10, m["display"].toString().toUtf8().constData(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (ins,11, m["time"].toLongLong());
        sqlite3_bind_text  (ins,12, m["attachTitle"].toString().toUtf8().constData(),-1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (ins,13, m["attachSize"].toLongLong());
        sqlite3_bind_text  (ins,14, m["attachExt"].toString().toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins,15, m["attachUrl"].toString().toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins,16, m["attachMime"].toString().toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (ins,17, m["attachMd5"].toString().toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);
    return execSql(db, "COMMIT", nullptr);
}

bool CacheDb::replaceMessages(const QString& accId, const QString& talker,
                               const QList<QVariantMap>& rows) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    const bool ok = replaceMessagesInternal(db, accId, talker, rows);
    closeDb(db);
    return ok;
}

bool upsertMessagesInternal(sqlite3* db, const QString& accId, const QString& talker,
                             const QList<QVariantMap>& rows) {
    const QString sql = R"SQL(
        INSERT INTO messages (acc_id, talker, msg_id, sender_id, sender_name, is_sender,
                              type, sub_type, content, display, time,
                              attach_title, attach_size, attach_ext, attach_url, attach_mime,
                              attach_md5)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17)
        ON CONFLICT(acc_id, msg_id) DO UPDATE SET
            sender_id=excluded.sender_id, sender_name=excluded.sender_name,
            content=excluded.content, display=excluded.display,
            type=excluded.type, sub_type=excluded.sub_type, time=excluded.time,
            attach_title=excluded.attach_title, attach_size=excluded.attach_size,
            attach_ext=excluded.attach_ext, attach_url=excluded.attach_url,
            attach_mime=excluded.attach_mime,
            attach_md5=excluded.attach_md5
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    for (const auto& m : rows) {
        sqlite3_reset(stmt);
        sqlite3_bind_text  (stmt, 1, accId.toUtf8().constData(),                -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, talker.toUtf8().constData(),               -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 3, m["msgId"].toLongLong());
        sqlite3_bind_text  (stmt, 4, m["senderId"].toString().toUtf8().constData(),   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 5, m["senderName"].toString().toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (stmt, 6, m["isSender"].toBool() ? 1 : 0);
        sqlite3_bind_int   (stmt, 7, m["type"].toInt());
        sqlite3_bind_int   (stmt, 8, m["subType"].toInt());
        sqlite3_bind_text  (stmt, 9, m["content"].toString().toUtf8().constData(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt,10, m["display"].toString().toUtf8().constData(),    -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt,11, m["time"].toLongLong());
        sqlite3_bind_text  (stmt,12, m["attachTitle"].toString().toUtf8().constData(),-1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt,13, m["attachSize"].toLongLong());
        sqlite3_bind_text  (stmt,14, m["attachExt"].toString().toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt,15, m["attachUrl"].toString().toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt,16, m["attachMime"].toString().toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt,17, m["attachMd5"].toString().toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return true;
}

bool CacheDb::upsertMessages(const QString& accId, const QString& talker,
                              const QList<QVariantMap>& rows) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    const bool ok = upsertMessagesInternal(db, accId, talker, rows);
    closeDb(db);
    return ok;
}

QList<QVariantMap> CacheDb::loadMessages(const QString& accId, const QString& talker,
                                          int limit) {
    QList<QVariantMap> out;
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return out;
    QString sql =
        "SELECT msg_id, sender_id, sender_name, is_sender, type, sub_type, "
        "content, display, time, "
        "attach_title, attach_size, attach_ext, attach_url, attach_mime, attach_md5 "
        "FROM messages WHERE acc_id=?1 AND talker=?2 ORDER BY time ASC";
    if (limit > 0) sql += QString(" LIMIT %1").arg(limit);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, talker.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            QVariantMap m;
            m["msgId"]      = qint64(sqlite3_column_int64(stmt, 0));
            m["senderId"]   = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 1));
            m["senderName"] = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 2));
            m["isSender"]   = sqlite3_column_int(stmt, 3) != 0;
            m["type"]       = sqlite3_column_int(stmt, 4);
            m["subType"]    = sqlite3_column_int(stmt, 5);
            m["content"]    = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 6));
            m["display"]    = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 7));
            m["time"]       = qint64(sqlite3_column_int64(stmt, 8));
            m["attachTitle"] = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 9));
            m["attachSize"]  = qint64(sqlite3_column_int64(stmt, 10));
            m["attachExt"]   = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 11));
            m["attachUrl"]   = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 12));
            m["attachMime"]  = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 13));
            m["attachMd5"]   = QString::fromUtf8((const char*)sqlite3_column_text(stmt, 14));
            out.append(m);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return out;
}

// ── 群成员 ──────────────────────────────────────────────────────────

bool CacheDb::replaceChatRoomMembers(const QString& accId, const QString& chatRoomId,
                                       const QStringList& wxids) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    execSql(db, "BEGIN", nullptr);
    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM chat_room_members WHERE acc_id=?1 AND chat_room_id=?2",
                       -1, &del, nullptr);
    sqlite3_bind_text(del, 1, accId.toUtf8().constData(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(del, 2, chatRoomId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_finalize(del);

    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO chat_room_members (acc_id, chat_room_id, wxid) "
                           "VALUES (?1, ?2, ?3)",
                       -1, &ins, nullptr);
    for (const QString& wxid : wxids) {
        sqlite3_reset(ins);
        sqlite3_bind_text(ins, 1, accId.toUtf8().constData(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, chatRoomId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, wxid.toUtf8().constData(),       -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);
    const bool ok = execSql(db, "COMMIT", nullptr);
    closeDb(db);
    return ok;
}

QStringList CacheDb::loadChatRoomMembers(const QString& accId, const QString& chatRoomId) {
    QStringList out;
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT wxid FROM chat_room_members "
                                "WHERE acc_id=?1 AND chat_room_id=?2 ORDER BY wxid",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, chatRoomId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW)
            out << QString::fromUtf8((const char*)sqlite3_column_text(stmt, 0));
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return out;
}

// ── 同步状态 ────────────────────────────────────────────────────────

bool CacheDb::getSyncState(const QString& accId, const QString& sourcePath,
                            qint64* sizeOut, qint64* mtimeOut) {
    bool found = false;
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT source_size, source_mtime FROM sync_state "
                                "WHERE acc_id=?1 AND source_path=?2",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, sourcePath.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            if (sizeOut)  *sizeOut  = sqlite3_column_int64(stmt, 0);
            if (mtimeOut) *mtimeOut = sqlite3_column_int64(stmt, 1);
            found = true;
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return found;
}

bool CacheDb::setSyncState(const QString& accId, const QString& sourcePath,
                            qint64 size, qint64 mtime) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    const QString sql = R"SQL(
        INSERT INTO sync_state (acc_id, source_path, source_size, source_mtime, synced_at)
        VALUES (?1, ?2, ?3, ?4, strftime('%s','now'))
        ON CONFLICT(acc_id, source_path) DO UPDATE SET
            source_size=excluded.source_size,
            source_mtime=excluded.source_mtime,
            synced_at=excluded.synced_at
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    bool ok = (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK);
    if (ok) {
        sqlite3_bind_text  (stmt, 1, accId.toUtf8().constData(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text  (stmt, 2, sourcePath.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64 (stmt, 3, size);
        sqlite3_bind_int64 (stmt, 4, mtime);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return ok;
}

bool CacheDb::clearSyncState(const QString& accId) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    sqlite3_stmt* stmt = nullptr;
    bool ok = (sqlite3_prepare_v2(db, "DELETE FROM sync_state WHERE acc_id=?1",
                                  -1, &stmt, nullptr) == SQLITE_OK);
    if (ok) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
    // 同时清掉内容指纹，避免后续 sync 误判"未变"
    if (sqlite3_prepare_v2(db, "DELETE FROM sync_content_hash WHERE acc_id=?1",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return ok;
}

// ── 内容指纹（信号层去重） ─────────────────────────────────────────

QString CacheDb::getContentHash(const QString& accId, const QString& kind) {
    QString out;
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT hash FROM sync_content_hash "
                                "WHERE acc_id=?1 AND kind=?2",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, kind.toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(stmt, 0);
            if (t) out = QString::fromUtf8((const char*)t);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return out;
}

bool CacheDb::setContentHash(const QString& accId, const QString& kind,
                              const QString& hash) {
    QMutexLocker lock(&g_writeMutex);
    sqlite3* db = nullptr;
    if (!openDb(&db, nullptr)) return false;
    const QString sql = R"SQL(
        INSERT INTO sync_content_hash (acc_id, kind, hash, synced_at)
        VALUES (?1, ?2, ?3, strftime('%s','now'))
        ON CONFLICT(acc_id, kind) DO UPDATE SET
            hash=excluded.hash, synced_at=excluded.synced_at
    )SQL";
    sqlite3_stmt* stmt = nullptr;
    bool ok = (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) == SQLITE_OK);
    if (ok) {
        sqlite3_bind_text(stmt, 1, accId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, kind.toUtf8().constData(),  -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, hash.toUtf8().constData(),   -1, SQLITE_TRANSIENT);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
    }
    if (stmt) sqlite3_finalize(stmt);
    closeDb(db);
    return ok;
}