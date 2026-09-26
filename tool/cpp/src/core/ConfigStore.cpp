#include "core/ConfigStore.h"
#include "core/Logger.h"
#include "sqlite3.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QDateTime>

ConfigStore& ConfigStore::instance() {
    static ConfigStore s;
    return s;
}

ConfigStore::ConfigStore() {
    ensureOpen();
}

ConfigStore::~ConfigStore() {
    if (m_db) {
        sqlite3_close(reinterpret_cast<sqlite3*>(m_db));
        m_db = nullptr;
    }
}

bool ConfigStore::ensureOpen() {
    if (m_db) return true;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString dbPath = dir + "/config.db";

    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.toUtf8().constData(), &db) != SQLITE_OK) {
        Logger::instance().error(
            QString("ConfigStore: 打开 config.db 失败: %1")
                .arg(db ? QString::fromUtf8(sqlite3_errmsg(db)) : "未知错误"), "core");
        if (db) sqlite3_close(db);
        return false;
    }
    m_db = db;

    char* err = nullptr;
    if (sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS config ("
            "  domain     TEXT PRIMARY KEY,"
            "  value      TEXT NOT NULL,"
            "  updated_at TEXT"
            ")", nullptr, nullptr, &err) != SQLITE_OK) {
        Logger::instance().error(
            QString("ConfigStore: 建表失败: %1")
                .arg(err ? QString::fromUtf8(err) : "未知错误"), "core");
        if (err) sqlite3_free(err);
        return false;
    }
    Logger::instance().info(
        QString("ConfigStore: config.db 已打开 (%1)").arg(dbPath), "core");
    return true;
}

QByteArray ConfigStore::readDomain(const QString& domain) {
    if (!ensureOpen()) return QByteArray();

    sqlite3* db = reinterpret_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;
    QByteArray out;

    if (sqlite3_prepare_v2(db,
                           "SELECT value FROM config WHERE domain = ?",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        const QByteArray dom = domain.toUtf8();
        sqlite3_bind_text(stmt, 1, dom.constData(), dom.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW
            && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const int len = sqlite3_column_bytes(stmt, 0);
            if (v && len > 0)
                out = QByteArray(v, len);
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return out;
}

bool ConfigStore::writeDomain(const QString& domain, const QByteArray& json) {
    if (!ensureOpen()) return false;

    sqlite3* db = reinterpret_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;

    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO config (domain, value, updated_at) VALUES (?, ?, ?)",
            -1, &stmt, nullptr) == SQLITE_OK) {
        const QByteArray dom = domain.toUtf8();
        const QByteArray now = QDateTime::currentDateTime()
                                   .toString(Qt::ISODate).toUtf8();
        sqlite3_bind_text(stmt, 1, dom.constData(), dom.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, json.constData(), json.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, now.constData(), now.size(), SQLITE_TRANSIENT);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
    }
    if (stmt) sqlite3_finalize(stmt);
    return ok;
}

bool ConfigStore::migrateFromFile(const QString& domain, const QString& jsonPath) {
    if (!ensureOpen()) return false;
    const bool dbHas = !readDomain(domain).isEmpty();

    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) return dbHas;   // 文件不存在：DB 有数据即迁移完成
    const QByteArray data = f.readAll();
    f.close();   // Windows：文件打开状态下 rename 会失败，必须先关闭

    const QString bak = jsonPath + ".migrated.bak";
    if (dbHas) {
        // DB 已有数据但 JSON 残留（上次 rename 失败等）→ 补做备份改名
        QFile::remove(bak);
        QFile::rename(jsonPath, bak);
        return false;
    }
    if (data.trimmed().isEmpty()) return false;
    if (!writeDomain(domain, data)) return false;

    QFile::remove(bak);
    if (!QFile::rename(jsonPath, bak)) {
        Logger::instance().warn(
            QString("ConfigStore: 域 '%1' 已入库但旧文件改名失败: %2").arg(domain, jsonPath), "core");
        return true;
    }
    Logger::instance().info(
        QString("ConfigStore: 域 '%1' 已从 JSON 迁移至 SQLite (备份: %2)")
            .arg(domain, bak), "core");
    return true;
}
