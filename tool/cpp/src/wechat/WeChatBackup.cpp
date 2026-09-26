#include "wechat/WeChatBackup.h"

#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

#include "sqlite3.h"

// 全局互斥：全量备份 / 会话增量同步可能在不同线程并发写同一备份库
static QMutex g_backupMtx;

WeChatBackup::WeChatBackup(const QString& accountId)
    : m_accountId(accountId), m_path(backupPath(accountId)) {}

WeChatBackup::~WeChatBackup() {
    if (m_db) {
        sqlite3_close(static_cast<sqlite3*>(m_db));
        m_db = nullptr;
    }
}

QString WeChatBackup::backupPath(const QString& accountId) {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + "/wechat_backup/" + accountId + ".db";
}

bool WeChatBackup::open() {
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    sqlite3* db = nullptr;
    if (sqlite3_open(m_path.toUtf8().constData(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        m_lastError = "备份库打开失败: " + m_path;
        return false;
    }
    m_db = db;
    // WAL + busy_timeout：多线程读写下不轻易报 SQLITE_BUSY
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);

    const char* kSchema =
        "CREATE TABLE IF NOT EXISTS messages("
        "  talker     TEXT    NOT NULL,"
        "  local_key  INTEGER NOT NULL,"       // v4=sort_seq / v3=localId
        "  msg_id     INTEGER NOT NULL DEFAULT 0,"
        "  is_sender  INTEGER NOT NULL DEFAULT 0,"
        "  type       INTEGER NOT NULL DEFAULT 1,"
        "  sub_type   INTEGER NOT NULL DEFAULT 0,"
        "  sender_id  TEXT    NOT NULL DEFAULT '',"
        "  sender_name TEXT   NOT NULL DEFAULT '',"
        "  content    TEXT    NOT NULL DEFAULT '',"
        "  display    TEXT    NOT NULL DEFAULT '',"
        "  time       INTEGER NOT NULL,"      // epoch 秒
        "  PRIMARY KEY(talker, local_key, time)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_msg_talker_time"
        "  ON messages(talker, time);"
        "CREATE TABLE IF NOT EXISTS meta("
        "  key TEXT PRIMARY KEY, value TEXT"
        ");";
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
        m_lastError = QString("备份库初始化失败: ")
                          + sqlite3_errmsg(db);
        sqlite3_close(db);
        m_db = nullptr;
        return false;
    }
    return true;
}

bool WeChatBackup::exec(const QString& sql) {
    sqlite3* db = static_cast<sqlite3*>(m_db);
    return sqlite3_exec(db, sql.toUtf8().constData(),
                        nullptr, nullptr, nullptr) == SQLITE_OK;
}

qint64 WeChatBackup::syncMessages(const QList<WeChatDb::ChatMessage>& msgs) {
    if (msgs.isEmpty()) return 0;
    if (!m_db) {
        m_lastError = "备份库未打开";
        return -1;
    }
    QMutexLocker lock(&g_backupMtx);
    sqlite3* db = static_cast<sqlite3*>(m_db);

    if (!exec("BEGIN IMMEDIATE")) {
        m_lastError = QString("备份事务开启失败: ") + sqlite3_errmsg(db);
        return -1;
    }

    const char* kInsert =
        "INSERT OR IGNORE INTO messages"
        "(talker, local_key, msg_id, is_sender, type, sub_type,"
        " sender_id, sender_name, content, display, time) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kInsert, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = QString("备份写入准备失败: ") + sqlite3_errmsg(db);
        exec("ROLLBACK");
        return -1;
    }

    qint64 inserted = 0;
    for (const auto& m : msgs) {
        if (m.talker.isEmpty()) continue;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, m.talker.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, m.localKey);
        sqlite3_bind_int64(stmt, 3, m.msgId);
        sqlite3_bind_int(stmt, 4, m.isSender ? 1 : 0);
        sqlite3_bind_int(stmt, 5, m.type);
        sqlite3_bind_int(stmt, 6, m.subType);
        sqlite3_bind_text(stmt, 7, m.senderId.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, m.senderName.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, m.content.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, m.display.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 11, m.time.toSecsSinceEpoch());
        if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0)
            ++inserted;
    }
    sqlite3_finalize(stmt);

    if (!exec("COMMIT")) {
        m_lastError = QString("备份事务提交失败: ") + sqlite3_errmsg(db);
        exec("ROLLBACK");
        return -1;
    }
    return inserted;
}

QList<WeChatDb::ChatMessage> WeChatBackup::messages(const QString& talker) {
    QList<WeChatDb::ChatMessage> out;
    if (!m_db || talker.isEmpty()) return out;
    QMutexLocker lock(&g_backupMtx);
    sqlite3* db = static_cast<sqlite3*>(m_db);

    const char* kSelect =
        "SELECT local_key, msg_id, is_sender, type, sub_type, sender_id,"
        "       sender_name, content, display, time "
        "FROM messages WHERE talker = ?1 ORDER BY time, local_key";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSelect, -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = QString("备份读取失败: ") + sqlite3_errmsg(db);
        return out;
    }
    sqlite3_bind_text(stmt, 1, talker.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WeChatDb::ChatMessage m;
        m.localKey   = sqlite3_column_int64(stmt, 0);
        m.msgId      = sqlite3_column_int64(stmt, 1);
        m.isSender   = sqlite3_column_int(stmt, 2) != 0;
        m.type       = sqlite3_column_int(stmt, 3);
        m.subType    = sqlite3_column_int(stmt, 4);
        m.senderId   = QString::fromUtf8(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
        m.senderName = QString::fromUtf8(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)));
        m.content    = QString::fromUtf8(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)));
        m.display    = QString::fromUtf8(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)));
        m.talker     = talker;
        m.time       = QDateTime::fromSecsSinceEpoch(sqlite3_column_int64(stmt, 9));
        out.append(m);
    }
    sqlite3_finalize(stmt);
    return out;
}

qint64 WeChatBackup::messageCount() {
    if (!m_db) return -1;
    QMutexLocker lock(&g_backupMtx);
    sqlite3* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM messages", -1,
                           &stmt, nullptr) != SQLITE_OK)
        return -1;
    qint64 n = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        n = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

// 通用行 → ChatMessage（列序与 messages() 的 SELECT 一致）
static WeChatDb::ChatMessage rowToMessage(sqlite3_stmt* stmt, int baseCol = 0) {
    WeChatDb::ChatMessage m;
    m.localKey   = sqlite3_column_int64(stmt, baseCol + 0);
    m.msgId      = sqlite3_column_int64(stmt, baseCol + 1);
    m.isSender   = sqlite3_column_int(stmt, baseCol + 2) != 0;
    m.type       = sqlite3_column_int(stmt, baseCol + 3);
    m.subType    = sqlite3_column_int(stmt, baseCol + 4);
    m.senderId   = QString::fromUtf8(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, baseCol + 5)));
    m.senderName = QString::fromUtf8(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, baseCol + 6)));
    m.content    = QString::fromUtf8(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, baseCol + 7)));
    m.display    = QString::fromUtf8(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, baseCol + 8)));
    m.talker     = QString::fromUtf8(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, baseCol + 9)));
    m.time       = QDateTime::fromSecsSinceEpoch(sqlite3_column_int64(stmt, baseCol + 10));
    return m;
}

QList<WeChatDb::ChatMessage> WeChatBackup::searchMessages(const QString& keyword,
                                                          const QString& talker,
                                                          int limit) {
    QList<WeChatDb::ChatMessage> out;
    if (!m_db || keyword.trimmed().isEmpty() || limit <= 0) return out;

    // LIKE 转义：% _ \ 按字面匹配
    QString kw = keyword.trimmed();
    QString esc;
    esc.reserve(kw.size());
    for (const QChar ch : kw) {
        if (ch == u'%' || ch == u'_' || ch == u'\\') esc += u'\\';
        esc += ch;
    }
    const QString pattern = "%" + esc + "%";

    QString sql =
        "SELECT local_key, msg_id, is_sender, type, sub_type, sender_id,"
        "       sender_name, content, display, talker, time "
        "FROM messages WHERE (content LIKE ?1 ESCAPE '\\' "
        "OR display LIKE ?1 ESCAPE '\\')";
    if (!talker.isEmpty()) sql += " AND talker = ?2";
    sql += " ORDER BY time DESC, local_key LIMIT " + QString::number(limit);

    QMutexLocker lock(&g_backupMtx);
    sqlite3* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = QString("搜索准备失败: ") + sqlite3_errmsg(db);
        return out;
    }
    sqlite3_bind_text(stmt, 1, pattern.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    if (!talker.isEmpty())
        sqlite3_bind_text(stmt, 2, talker.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out.append(rowToMessage(stmt));
    sqlite3_finalize(stmt);
    return out;
}

QList<WeChatDb::ChatMessage> WeChatBackup::attachmentMessages(const QString& talker,
                                                               int limit) {
    QList<WeChatDb::ChatMessage> out;
    if (!m_db || limit <= 0) return out;

    QString sql =
        "SELECT local_key, msg_id, is_sender, type, sub_type, sender_id,"
        "       sender_name, content, display, talker, time "
        "FROM messages WHERE (type IN (3,34,43,47) OR (type = 49 AND sub_type = 4))";
    if (!talker.isEmpty()) sql += " AND talker = ?1";
    sql += " ORDER BY time DESC, local_key LIMIT " + QString::number(limit);

    QMutexLocker lock(&g_backupMtx);
    sqlite3* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.toUtf8().constData(), -1, &stmt, nullptr) != SQLITE_OK) {
        m_lastError = QString("附件查询失败: ") + sqlite3_errmsg(db);
        return out;
    }
    if (!talker.isEmpty())
        sqlite3_bind_text(stmt, 1, talker.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out.append(rowToMessage(stmt));
    sqlite3_finalize(stmt);
    return out;
}
