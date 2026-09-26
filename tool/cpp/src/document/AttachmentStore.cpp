#include "document/AttachmentStore.h"
#include "core/Logger.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QBuffer>
#include <QUuid>
#include <QMimeDatabase>
#include <QMimeType>
#include <QVariant>

AttachmentStore& AttachmentStore::instance() {
    static AttachmentStore s;
    return s;
}

// 在 cpp 中使用嵌套类型 Attachment 时简化写法
using Attachment = AttachmentStore::Attachment;

AttachmentStore::AttachmentStore(QObject* parent) : QObject(parent) {}
AttachmentStore::~AttachmentStore() = default;

void AttachmentStore::init() {
    if (m_initialized) return;

    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    m_dbPath = base + "/attachments.db";
    m_storageDir = base + "/attachments";
    QDir().mkpath(m_storageDir);

    m_connName = QString("attachments_%1").arg(QUuid::createUuid().toString(QUuid::Id128));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connName);
        db.setDatabaseName(m_dbPath);
        if (!db.open()) {
            Logger::instance().error(
                QString("打开附件数据库失败: %1").arg(db.lastError().text()), "doc");
            return;
        }
    }
    if (!ensureSchema()) {
        QSqlDatabase::removeDatabase(m_connName);
        m_connName.clear();
        return;
    }
    m_initialized = true;
    Logger::instance().info(
        QString("附件存储已就绪: %1").arg(m_storageDir), "doc");
}

bool AttachmentStore::ensureSchema() {
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    QSqlQuery q(db);
    const char* ddl =
        "CREATE TABLE IF NOT EXISTS attachments ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  sha256        TEXT NOT NULL,"
        "  original_name TEXT NOT NULL,"
        "  stored_path   TEXT NOT NULL,"
        "  mime_type     TEXT,"
        "  size          INTEGER NOT NULL,"
        "  source        TEXT NOT NULL,"
        "  source_ref    TEXT,"
        "  account_id    TEXT,"
        "  tags          TEXT,"
        "  description   TEXT,"
        "  uploaded_at   TEXT NOT NULL,"
        "  UNIQUE(sha256, original_name)"
        ")";
    if (!q.exec(ddl)) {
        Logger::instance().error(
            QString("创建附件表失败: %1").arg(q.lastError().text()), "doc");
        return false;
    }
    q.exec("CREATE INDEX IF NOT EXISTS idx_attachments_sha256 ON attachments(sha256)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_attachments_source ON attachments(source)");
    q.exec("CREATE INDEX IF NOT EXISTS idx_attachments_uploaded ON attachments(uploaded_at)");
    return true;
}

QString AttachmentStore::storeBytes(const QByteArray& data, const QString& fileName, QString* errorMessage) {
    const QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
    // 用 sha256 前 2 位做一级目录，避免单目录文件过多
    const QString subDir = m_storageDir + "/" + QString::fromLatin1(hash.left(2));
    QDir().mkpath(subDir);

    QString ext = QFileInfo(fileName).suffix();
    QString fileName2 = QString::fromLatin1(hash) + (ext.isEmpty() ? QString() : "." + ext);
    QString absPath = subDir + "/" + fileName2;

    if (!QFile::exists(absPath)) {
        QFile f(absPath);
        if (!f.open(QIODevice::WriteOnly)) {
            if (errorMessage) *errorMessage = QString("无法写入存储文件: %1").arg(f.errorString());
            return {};
        }
        if (f.write(data) != data.size()) {
            if (errorMessage) *errorMessage = "写入存储文件时字节数不匹配";
            return {};
        }
        f.close();
    }
    // 返回相对路径（统一以 "/" 分隔）
    QString rel = QString::fromLatin1(hash.left(2)) + "/" + fileName2;
    return rel;
}

qint64 AttachmentStore::importFile(const QString& sourceFilePath,
                                   const QString& source,
                                   const QString& sourceRef,
                                   const QString& accountId,
                                   const QString& tags,
                                   const QString& description,
                                   QString* errorMessage) {
    if (!m_initialized) init();
    QFile in(sourceFilePath);
    if (!in.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = QString("无法读取源文件: %1").arg(in.errorString());
        return -1;
    }
    QByteArray data = in.readAll();
    in.close();
    return importBytes(data, QFileInfo(sourceFilePath).fileName(),
                       source, sourceRef, accountId, tags, description, errorMessage);
}

qint64 AttachmentStore::importBytes(const QByteArray& data,
                                    const QString& fileName,
                                    const QString& source,
                                    const QString& sourceRef,
                                    const QString& accountId,
                                    const QString& tags,
                                    const QString& description,
                                    QString* errorMessage) {
    if (!m_initialized) init();
    if (data.isEmpty()) {
        if (errorMessage) *errorMessage = "空文件";
        return -1;
    }

    QString err;
    QString relPath = storeBytes(data, fileName, &err);
    if (relPath.isEmpty()) {
        if (errorMessage) *errorMessage = err;
        return -1;
    }

    const QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
    QMimeDatabase mdb;
    QString mime = mdb.mimeTypeForFileNameAndData(fileName, data).name();

    QSqlDatabase db = QSqlDatabase::database(m_connName);
    QSqlQuery q(db);
    q.prepare(
        "INSERT INTO attachments"
        " (sha256, original_name, stored_path, mime_type, size,"
        "  source, source_ref, account_id, tags, description, uploaded_at)"
        " VALUES (:sha, :name, :path, :mime, :size,"
        "         :src, :ref, :acct, :tags, :desc, :ts)");
    q.bindValue(":sha",  QString::fromLatin1(hash));
    q.bindValue(":name", fileName);
    q.bindValue(":path", relPath);
    q.bindValue(":mime", mime);
    q.bindValue(":size", data.size());
    q.bindValue(":src",  source.isEmpty() ? "manual" : source);
    q.bindValue(":ref",  sourceRef);
    q.bindValue(":acct", accountId);
    q.bindValue(":tags", tags);
    q.bindValue(":desc", description);
    q.bindValue(":ts",   QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (!q.exec()) {
        if (q.lastError().text().contains("UNIQUE", Qt::CaseInsensitive)) {
            // 同名同 sha 已存在：查找现有记录 id
            QSqlQuery q2(db);
            q2.prepare("SELECT id FROM attachments WHERE sha256=:sha AND original_name=:name");
            q2.bindValue(":sha",  QString::fromLatin1(hash));
            q2.bindValue(":name", fileName);
            if (q2.exec() && q2.next()) {
                emit changed();
                return q2.value(0).toLongLong();
            }
        }
        if (errorMessage) *errorMessage = QString("写入元数据失败: %1").arg(q.lastError().text());
        Logger::instance().error(
            QString("导入附件失败: %1").arg(q.lastError().text()), "doc");
        return -1;
    }
    emit changed();
    Logger::instance().info(
        QString("导入附件: %1 (%2 字节) -> id=%3").arg(fileName).arg(data.size()).arg(q.lastInsertId().toLongLong()),
        "doc");
    return q.lastInsertId().toLongLong();
}

Attachment AttachmentStore::rowToAttachment(void* stmtHandle) const {
    Attachment a;
    auto* q = static_cast<QSqlQuery*>(stmtHandle);
    QSqlRecord r = q->record();
    a.id           = q->value(r.indexOf("id")).toLongLong();
    a.sha256       = q->value(r.indexOf("sha256")).toString();
    a.originalName = q->value(r.indexOf("original_name")).toString();
    a.storedPath   = q->value(r.indexOf("stored_path")).toString();
    a.mimeType     = q->value(r.indexOf("mime_type")).toString();
    a.size         = q->value(r.indexOf("size")).toLongLong();
    a.source       = q->value(r.indexOf("source")).toString();
    a.sourceRef    = q->value(r.indexOf("source_ref")).toString();
    a.accountId    = q->value(r.indexOf("account_id")).toString();
    a.tags         = q->value(r.indexOf("tags")).toString();
    a.description  = q->value(r.indexOf("description")).toString();
    a.uploadedAt   = QDateTime::fromString(q->value(r.indexOf("uploaded_at")).toString(), Qt::ISODate);
    return a;
}

QList<AttachmentStore::Attachment> AttachmentStore::list(const Filter& f) const {
    QList<Attachment> out;
    if (!m_initialized) return out;
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    QSqlQuery q(db);
    QString sql = "SELECT * FROM attachments WHERE 1=1";
    if (!f.source.isEmpty())   sql += " AND source=:src";
    if (!f.keyword.isEmpty())  sql += " AND (original_name LIKE :kw OR tags LIKE :kw OR description LIKE :kw)";
    if (!f.tag.isEmpty())      sql += " AND tags LIKE :tag";
    sql += " ORDER BY uploaded_at DESC, id DESC";
    q.prepare(sql);
    if (!f.source.isEmpty())  q.bindValue(":src", f.source);
    if (!f.keyword.isEmpty()) q.bindValue(":kw", "%" + f.keyword + "%");
    if (!f.tag.isEmpty())     q.bindValue(":tag", "%," + f.tag + ",%");
    if (!q.exec()) {
        Logger::instance().error(
            QString("查询附件失败: %1").arg(q.lastError().text()), "doc");
        return out;
    }
    while (q.next()) out.append(rowToAttachment(&q));
    return out;
}

int AttachmentStore::count(const Filter& f) const {
    if (!m_initialized) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    QSqlQuery q(db);
    QString sql = "SELECT COUNT(*) FROM attachments WHERE 1=1";
    if (!f.source.isEmpty())  sql += " AND source=:src";
    if (!f.keyword.isEmpty()) sql += " AND (original_name LIKE :kw OR tags LIKE :kw OR description LIKE :kw)";
    if (!f.tag.isEmpty())     sql += " AND tags LIKE :tag";
    q.prepare(sql);
    if (!f.source.isEmpty())  q.bindValue(":src", f.source);
    if (!f.keyword.isEmpty()) q.bindValue(":kw", "%" + f.keyword + "%");
    if (!f.tag.isEmpty())     q.bindValue(":tag", "%," + f.tag + ",%");
    if (!q.exec() || !q.next()) return 0;
    return q.value(0).toInt();
}

Attachment AttachmentStore::getById(qint64 id) const {
    if (!m_initialized) return {};
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    QSqlQuery q(db);
    q.prepare("SELECT * FROM attachments WHERE id=:id");
    q.bindValue(":id", id);
    if (!q.exec() || !q.next()) return {};
    return rowToAttachment(&q);
}

qint64 AttachmentStore::totalSize() const {
    if (!m_initialized) return 0;
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    QSqlQuery q(db);
    if (!q.exec("SELECT COALESCE(SUM(size),0) FROM attachments") || !q.next()) return 0;
    return q.value(0).toLongLong();
}

bool AttachmentStore::remove(qint64 id, QString* errorMessage) {
    if (!m_initialized) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connName);

    // 取 sha256 之后再删
    QString sha;
    {
        QSqlQuery q(db);
        q.prepare("SELECT sha256 FROM attachments WHERE id=:id");
        q.bindValue(":id", id);
        if (!q.exec() || !q.next()) {
            if (errorMessage) *errorMessage = "记录不存在";
            return false;
        }
        sha = q.value(0).toString();
    }

    QSqlQuery del(db);
    del.prepare("DELETE FROM attachments WHERE id=:id");
    del.bindValue(":id", id);
    if (!del.exec()) {
        if (errorMessage) *errorMessage = del.lastError().text();
        return false;
    }

    // 若 sha256 已无引用，删物理文件
    QSqlQuery ref(db);
    ref.prepare("SELECT COUNT(*) FROM attachments WHERE sha256=:sha");
    ref.bindValue(":sha", sha);
    if (ref.exec() && ref.next() && ref.value(0).toInt() == 0) {
        QDir d(m_storageDir + "/" + sha.left(2));
        if (d.exists()) {
            const QStringList files = d.entryList(QStringList{} << (sha + "*"), QDir::Files);
            for (const QString& f : files) d.remove(f);
        }
    }

    emit changed();
    return true;
}

bool AttachmentStore::updateTags(qint64 id, const QString& tags, QString* errorMessage) {
    if (!m_initialized) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    QSqlQuery q(db);
    q.prepare("UPDATE attachments SET tags=:t WHERE id=:id");
    q.bindValue(":t", tags.trimmed());
    q.bindValue(":id", id);
    if (!q.exec()) {
        if (errorMessage) *errorMessage = q.lastError().text();
        return false;
    }
    emit changed();
    return q.numRowsAffected() > 0;
}

bool AttachmentStore::updateDescription(qint64 id, const QString& description, QString* errorMessage) {
    if (!m_initialized) return false;
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    QSqlQuery q(db);
    q.prepare("UPDATE attachments SET description=:d WHERE id=:id");
    q.bindValue(":d", description);
    q.bindValue(":id", id);
    if (!q.exec()) {
        if (errorMessage) *errorMessage = q.lastError().text();
        return false;
    }
    emit changed();
    return q.numRowsAffected() > 0;
}

bool AttachmentStore::exportTo(qint64 id, const QString& destPath, QString* errorMessage) {
    Attachment a = getById(id);
    if (a.id == 0) {
        if (errorMessage) *errorMessage = "记录不存在";
        return false;
    }
    QString abs = m_storageDir + "/" + a.storedPath;
    if (!QFile::exists(abs)) {
        if (errorMessage) *errorMessage = "存储文件缺失: " + abs;
        return false;
    }
    if (QFile::exists(destPath)) QFile::remove(destPath);
    if (!QFile::copy(abs, destPath)) {
        if (errorMessage) *errorMessage = QString("复制失败: %1").arg(abs);
        return false;
    }
    return true;
}

QStringList AttachmentStore::allTags() const {
    QStringList out;
    if (!m_initialized) return out;
    QSqlDatabase db = QSqlDatabase::database(m_connName);
    QSqlQuery q(db);
    if (!q.exec("SELECT tags FROM attachments WHERE tags IS NOT NULL AND tags<>''")) return out;
    QSet<QString> set;
    while (q.next()) {
        const QStringList parts = q.value(0).toString().split(',', Qt::SkipEmptyParts);
        for (const QString& p : parts) set.insert(p.trimmed());
    }
    return QStringList(set.begin(), set.end());
}
