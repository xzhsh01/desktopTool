#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QVariantMap>
#include <QUuid>
#include <QDateTime>

/**
 * ConnectionManager: 应用管理（连接配置存储）
 * 对应原 src/stores/connection.ts 中的 Pinia store
 * 管理 SSH/数据库/Redis/RDP 连接配置，密码加密存储
 */
class ConnectionManager : public QObject {
    Q_OBJECT

public:
    enum ConnType { SSH, Database, Redis, RDP };
    Q_ENUM(ConnType)

    enum DbType { MySQL, PostgreSQL, SQLite, MSSQL, Oracle, MongoDB };
    Q_ENUM(DbType)

    struct Connection {
        QString id;
        QString name;
        ConnType type;
        QString host;
        int port = 0;
        QString username;
        QString password;       // 内存中为明文
        QString database;       // 数据库名/SQLite 文件路径
        QString connectionString; // 数据库连接串（ODBC 完整串，可选；优先于 host/port/database）
        DbType dbType = MySQL; // 仅 Database 类型有效
        QString authType = "password"; // SSH: "password" or "key"
        QString privateKey;     // SSH 私钥路径
        QString instantClientPath; // Oracle Instant Client 路径
        QString description;
        bool active = false;
        QDateTime createdAt;
        QDateTime updatedAt;
    };

    static ConnectionManager& instance();

    // 加载连接列表（从文件，解密密码）
    void load();
    // 保存连接列表（加密密码）
    void save();

    const QList<Connection>& connections() const { return m_connections; }
    Connection* getById(const QString& id);
    QList<Connection> getByType(ConnType type) const;
    int activeCount() const;

    Connection add(const QVariantMap& data);
    bool update(const QString& id, const QVariantMap& data);
    bool remove(const QString& id);
    void setActive(const QString& id, bool active);

    // 工具
    static QString connTypeToString(ConnType type);
    // 连接类型的中文显示名（仅 UI 展示，不影响序列化标识）
    static QString connTypeDisplayName(ConnType type);
    static QString dbTypeToString(DbType type);
    static ConnType stringToConnType(const QString& s);
    static DbType stringToDbType(const QString& s);
    static int defaultPort(const QString& typeOrDbType);

signals:
    void connectionsChanged();
    void activeChanged(const QString& id, bool active);

private:
    ConnectionManager(QObject* parent = nullptr);
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    QList<Connection> m_connections;
    QString m_filePath;

    // 密码加密前缀
    static constexpr const char* ENC_PREFIX = "enc:";
};
