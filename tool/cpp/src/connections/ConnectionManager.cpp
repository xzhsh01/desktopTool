#include "connections/ConnectionManager.h"
#include "core/Crypto.h"
#include <QStandardPaths>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QVariantHash>

ConnectionManager& ConnectionManager::instance() {
    static ConnectionManager cm;
    return cm;
}

ConnectionManager::ConnectionManager(QObject* parent) : QObject(parent) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    m_filePath = dir + "/connections.json";
    load();
}

void ConnectionManager::load() {
    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly)) return;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return;

    m_connections.clear();
    QJsonArray arr = doc.array();
    for (const auto& item : arr) {
        QJsonObject obj = item.toObject();
        Connection conn;
        conn.id = obj["id"].toString();
        conn.name = obj["name"].toString();
        conn.type = stringToConnType(obj["type"].toString());
        conn.host = obj["host"].toString();
        conn.port = obj["port"].toInt();
        conn.username = obj["username"].toString();

        // 解密密码
        QString storedPass = obj["password"].toString();
        if (storedPass.startsWith(ENC_PREFIX)) {
            conn.password = Crypto::instance().decrypt(storedPass.mid(QString(ENC_PREFIX).length()));
        } else {
            conn.password = storedPass; // 兼容历史明文
        }

        conn.database = obj["database"].toString();
        conn.connectionString = obj["connectionString"].toString();
        if (obj.contains("dbType"))
            conn.dbType = stringToDbType(obj["dbType"].toString());
        conn.authType = obj.value("authType").toString("password");
        conn.privateKey = obj["privateKey"].toString();
        conn.instantClientPath = obj["instantClientPath"].toString();
        conn.description = obj["description"].toString();
        conn.createdAt = QDateTime::fromString(obj["createdAt"].toString(), Qt::ISODate);
        conn.updatedAt = QDateTime::fromString(obj["updatedAt"].toString(), Qt::ISODate);

        m_connections.append(conn);
    }
}

void ConnectionManager::save() {
    QJsonArray arr;
    for (const auto& conn : m_connections) {
        QJsonObject obj;
        obj["id"] = conn.id;
        obj["name"] = conn.name;
        obj["type"] = connTypeToString(conn.type);
        obj["host"] = conn.host;
        obj["port"] = conn.port;
        obj["username"] = conn.username;

        // 加密密码
        if (!conn.password.isEmpty()) {
            QString enc = Crypto::instance().encrypt(conn.password);
            if (!enc.isEmpty()) {
                obj["password"] = QString(ENC_PREFIX) + enc;
            } else {
                obj["password"] = conn.password; // 降级明文
            }
        } else {
            obj["password"] = "";
        }

        obj["database"] = conn.database;
        obj["connectionString"] = conn.connectionString;
        obj["dbType"] = dbTypeToString(conn.dbType);
        obj["authType"] = conn.authType;
        obj["privateKey"] = conn.privateKey;
        obj["instantClientPath"] = conn.instantClientPath;
        obj["description"] = conn.description;
        obj["createdAt"] = conn.createdAt.toString(Qt::ISODate);
        obj["updatedAt"] = conn.updatedAt.toString(Qt::ISODate);
        arr.append(obj);
    }

    QFile f(m_filePath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonDocument doc(arr);
        f.write(doc.toJson(QJsonDocument::Indented));
    }
}

ConnectionManager::Connection* ConnectionManager::getById(const QString& id) {
    for (auto& c : m_connections) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

QList<ConnectionManager::Connection> ConnectionManager::getByType(ConnType type) const {
    QList<Connection> result;
    for (const auto& c : m_connections) {
        if (c.type == type) result.append(c);
    }
    return result;
}

int ConnectionManager::activeCount() const {
    int count = 0;
    for (const auto& c : m_connections) {
        if (c.active) count++;
    }
    return count;
}

ConnectionManager::Connection ConnectionManager::add(const QVariantMap& data) {
    Connection conn;
    conn.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    conn.name = data.value("name").toString();
    conn.type = stringToConnType(data.value("type").toString());
    conn.host = data.value("host").toString();
    conn.port = data.value("port").toInt();
    conn.username = data.value("username").toString();
    conn.password = data.value("password").toString();
    conn.database = data.value("database").toString();
    conn.connectionString = data.value("connectionString").toString();
    conn.dbType = stringToDbType(data.value("dbType", "mysql").toString());
    conn.authType = data.value("authType", "password").toString();
    conn.privateKey = data.value("privateKey").toString();
    conn.instantClientPath = data.value("instantClientPath").toString();
    conn.description = data.value("description").toString();
    conn.createdAt = conn.updatedAt = QDateTime::currentDateTime();

    m_connections.append(conn);
    save();
    emit connectionsChanged();
    return conn;
}

bool ConnectionManager::update(const QString& id, const QVariantMap& data) {
    for (auto& c : m_connections) {
        if (c.id == id) {
            if (data.contains("name")) c.name = data.value("name").toString();
            if (data.contains("type")) c.type = stringToConnType(data.value("type").toString());
            if (data.contains("host")) c.host = data.value("host").toString();
            if (data.contains("port")) c.port = data.value("port").toInt();
            if (data.contains("username")) c.username = data.value("username").toString();
            if (data.contains("password")) c.password = data.value("password").toString();
            if (data.contains("database")) c.database = data.value("database").toString();
            if (data.contains("connectionString")) c.connectionString = data.value("connectionString").toString();
            if (data.contains("dbType")) c.dbType = stringToDbType(data.value("dbType").toString());
            if (data.contains("authType")) c.authType = data.value("authType").toString();
            if (data.contains("privateKey")) c.privateKey = data.value("privateKey").toString();
            if (data.contains("instantClientPath")) c.instantClientPath = data.value("instantClientPath").toString();
            if (data.contains("description")) c.description = data.value("description").toString();
            c.updatedAt = QDateTime::currentDateTime();
            save();
            emit connectionsChanged();
            return true;
        }
    }
    return false;
}

bool ConnectionManager::remove(const QString& id) {
    for (int i = 0; i < m_connections.size(); ++i) {
        if (m_connections[i].id == id) {
            m_connections.removeAt(i);
            save();
            emit connectionsChanged();
            return true;
        }
    }
    return false;
}

void ConnectionManager::setActive(const QString& id, bool active) {
    for (auto& c : m_connections) {
        if (c.id == id) {
            c.active = active;
            emit activeChanged(id, active);
            return;
        }
    }
}

QString ConnectionManager::connTypeToString(ConnType type) {
    switch (type) {
        case SSH: return "ssh";
        case Database: return "database";
        case Redis: return "redis";
        case RDP: return "rdp";
    }
    return "ssh";
}

QString ConnectionManager::connTypeDisplayName(ConnType type) {
    switch (type) {
        case SSH: return "SSH 终端";
        case Database: return "数据库";
        case Redis: return "Redis";
        case RDP: return "远程桌面";
    }
    return "未知";
}

QString ConnectionManager::dbTypeToString(DbType type) {
    switch (type) {
        case MySQL: return "mysql";
        case PostgreSQL: return "postgres";
        case SQLite: return "sqlite";
        case MSSQL: return "mssql";
        case Oracle: return "oracle";
        case MongoDB: return "mongodb";
    }
    return "mysql";
}

ConnectionManager::ConnType ConnectionManager::stringToConnType(const QString& s) {
    if (s == "database") return Database;
    if (s == "redis") return Redis;
    if (s == "rdp") return RDP;
    return SSH;
}

ConnectionManager::DbType ConnectionManager::stringToDbType(const QString& s) {
    if (s == "postgres") return PostgreSQL;
    if (s == "sqlite") return SQLite;
    if (s == "mssql") return MSSQL;
    if (s == "oracle") return Oracle;
    if (s == "mongodb") return MongoDB;
    return MySQL;
}

int ConnectionManager::defaultPort(const QString& typeOrDbType) {
    static const QHash<QString, int> ports = {
        {"ssh", 22}, {"mysql", 3306}, {"postgres", 5432},
        {"mssql", 1433}, {"oracle", 1521}, {"redis", 6379},
        {"rdp", 3389}, {"mongodb", 27017}
    };
    return ports.value(typeOrDbType.toLower(), 0);
}
