#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QQueue>
#include <QVariantList>
#include <QSqlDatabase>
#include <QSqlQuery>

/**
 * DatabaseClient: 数据库客户端（Qt SQL 封装）
 * 对应原 electron/database.ts
 *
 * 支持的数据库（取决于编译时可用的 Qt SQL 驱动）：
 *  - MySQL      (QMYSQL)
 *  - PostgreSQL (QPSQL)
 *  - SQLite     (QSQLITE)
 *  - SQL Server (QODBC)
 *  - Oracle     (QOCI / QODBC)
 *
 * 注意：QSqlDatabase 连接只能在创建它的线程中使用，
 * 因此本客户端的所有操作固定在工作线程中执行。
 */
class DatabaseClient : public QObject {
    Q_OBJECT

public:
    // 查询结果
    struct QueryResult {
        bool success = false;
        bool cancelled = false;       // 被用户取消
        QString error;
        QStringList columns;
        QList<QVariantList> rows;     // 每行一个 QVariantList
        int affectedRows = 0;
        qint64 elapsedMs = 0;
        bool inTransaction = false;  // 执行后仍处于未提交事务中
        QString message;             // 事务操作提示（COMMIT/ROLLBACK/BEGIN）
    };

    // 字段详情（表结构-字段 tab / 编辑表结构用）
    struct ColumnDetail {
        QString name;
        QString type;           // 数据库原生类型（如 VARCHAR(64) / NUMBER(10,2)）
        bool primaryKey = false;
        bool autoIncrement = false;
        bool nullable = true;
        QString defaultValue;
        QString comment;
    };

    // 外键（表结构-外键 tab）
    struct ForeignKeyInfo {
        QString name;       // 外键约束名
        QString column;     // 本表字段
        QString refTable;   // 引用表
        QString refColumn;  // 引用字段
    };

    // 索引（表结构-索引 tab）
    struct IndexInfo {
        QString name;
        QString type;        // PRIMARY/UNIQUE/NORMAL/FULLTEXT/CLUSTERED...
        QStringList columns;
    };

    // 分区（表结构-分区 tab）
    struct PartitionInfo {
        QString name;
        QString type;    // RANGE/LIST/HASH/KEY...
        QString column;  // 分区字段/表达式
    };

    // 表元数据
    struct TableInfo {
        QString name;
        QString type = "table";  // table/view/procedure/function/package/packagebody/trigger/job
                                  // sequence/synonym/matview/type/dblink/index（按数据库类型）
        QString schema;          // 所属 schema（PostgreSQL schema / Oracle owner；其余为空）
        QStringList columns;
        QStringList columnTypes;
        QStringList primaryKeys;
        qint64 rowCount = -1;
        // 详细元数据（describeTable 时填充）
        QList<ColumnDetail> columnDetails;
        QList<ForeignKeyInfo> foreignKeys;
        QList<IndexInfo> indexes;
        QList<PartitionInfo> partitions;
    };

    struct ConnectParams {
        QString dbType;       // "mysql"/"postgres"/"sqlite"/"mssql"/"oracle"
        QString host;
        int port = 3306;
        QString username;
        QString password;
        QString database;     // 库名或 SQLite 文件路径
        QString connectionString; // ODBC 完整连接串（可选，优先于 host/port/database）
        QString instantClientPath;
        int timeoutSec = 30;
    };

    explicit DatabaseClient(QObject* parent = nullptr);
    ~DatabaseClient();

    // 公共 API
    void connectTo(const ConnectParams& params);
    void disconnect();
    void executeQuery(const QString& query);
    void cancelQuery();   // 取消当前正在执行的查询（线程安全）
    void listDatabases();
    void listTables(const QString& database = QString());
    void useDatabase(const QString& name);   // 切换当前数据库/用户（刷新对象树）
    void describeTable(const QString& table);
    void selectTableData(const QString& table, int limit = 200, int offset = 0);
    void getServerVersion();
    // 导出：流式查询整表，分块回传（配合 exportChunk/exportFinished 信号）
    void exportTableData(const QString& table);
    // 导入：执行一批 INSERT（sql 为整批文本），完成后发 scriptBatchDone
    void executeScriptBatch(const QString& sql, int batchIndex, int totalBatches);
    // 取对象定义 SQL（objectType: view/matview/procedure/function/trigger/
    // sequence/index/package/packagebody/synonym/type/dblink）
    void fetchObjectSql(const QString& objectType, const QString& object);

    bool isConnected() const { return m_connected; }

    // 驱动可用性检查
    static QStringList availableDrivers();
    static bool isDbTypeSupported(const QString& dbType);

signals:
    void connected(const QString& serverVersion);
    void disconnected();
    void connectionError(const QString& message);
    void queryStarted();  // 实际开始执行（worker 线程），用于精确计时
    void queryResult(const QString& query, const QueryResult& result);
    void databasesListed(const QStringList& databases, const QString& current);
    void tablesListed(const QList<TableInfo>& tables);
    void tableDescribed(const TableInfo& info);
    void statusMessage(const QString& message);
    // 导出进度（worker 流式回传）
    void exportColumnsReady(const QString& table, const QStringList& columns);
    void exportChunk(const QString& table, const QList<QVariantList>& rows, qint64 fetched);
    void exportFinished(const QString& table, const QString& error);
    // 导入批次完成
    void scriptBatchDone(int batchIndex, int totalBatches, bool ok, const QString& error);
    // 对象定义 SQL（除表、调度作业外的对象均可获取）
    void objectSqlFetched(const QString& objectType, const QString& object,
                          const QString& sql, const QString& error);

private:
    void setConnected(bool v) { m_connected = v; }

    class DatabaseWorker* m_worker = nullptr;
    bool m_connected = false;
};

Q_DECLARE_METATYPE(DatabaseClient::QueryResult)
Q_DECLARE_METATYPE(DatabaseClient::TableInfo)
Q_DECLARE_METATYPE(QList<DatabaseClient::TableInfo>)

/**
 * DatabaseWorker: 工作线程
 * QSqlDatabase 连接的创建和使用全部在此线程
 */
class DatabaseWorker : public QThread {
    Q_OBJECT

public:
    explicit DatabaseWorker(QObject* parent = nullptr);
    ~DatabaseWorker();

    void setConnectParams(const DatabaseClient::ConnectParams& params) { m_connectParams = params; }
    void queueCommand(int type, const QString& a = QString(), int n = 0, int n2 = 0);
    // 双字符串参数版本（如 CmdFetchObjectSql 的 objectType + objectName）
    void queueCommand(int type, const QString& a, const QString& b);
    void requestStop();
    // 请求取消当前正在执行的查询
    void requestCancel();

    enum CmdType {
        CmdConnect, CmdDisconnect, CmdQuery,
        CmdListDatabases, CmdListTables, CmdDescribeTable,
        CmdSelectData, CmdVersion, CmdUseDatabase,
        CmdExportData, CmdScriptBatch, CmdFetchObjectSql
    };

signals:
    void connected(const QString& serverVersion);
    void disconnected();
    void connectionError(const QString& message);
    void queryStarted();
    void queryResult(const QString& query, const DatabaseClient::QueryResult& result);
    void databasesListed(const QStringList& databases, const QString& current);
    void tablesListed(const QList<DatabaseClient::TableInfo>& tables);
    void tableDescribed(const DatabaseClient::TableInfo& info);
    void statusMessage(const QString& message);
    void exportColumnsReady(const QString& table, const QStringList& columns);
    void exportChunk(const QString& table, const QList<QVariantList>& rows, qint64 fetched);
    void exportFinished(const QString& table, const QString& error);
    void scriptBatchDone(int batchIndex, int totalBatches, bool ok, const QString& error);
    void objectSqlFetched(const QString& objectType, const QString& object,
                          const QString& sql, const QString& error);

protected:
    void run() override;

private:
    struct Command {
        int type;
        QString a;
        QString b;
        int n = 0;
        int n2 = 0;
    };

    bool doConnect(const DatabaseClient::ConnectParams& params);
    void doDisconnect();
    void doQuery(const QString& query);
    void doListDatabases();
    void doListTables();
    void doUseDatabase(const QString& name);
    void doDescribeTable(const QString& table);
    void doSelectData(const QString& table, int limit, int offset);
    void doVersion();
    void doExportData(const QString& table);
    void doScriptBatch(const QString& sql, int batchIndex, int totalBatches);
    void doFetchObjectSql(const QString& objectType, const QString& object);

    // Qt SQL 驱动名映射
    static QString driverForType(const QString& dbType);
    // 构建连接字符串（ODBC 类）
    static QString odbcConnectionString(const DatabaseClient::ConnectParams& params);
    // 枚举系统已安装的 ODBC 驱动，按关键字匹配（Windows 注册表）
    static QString findOdbcDriver(const QString& keyword, const QString& preferred = {});
    // 顶层节点显示格式：userName@host:port/database
    QString connDisplay(const QString& dbName) const;

    DatabaseClient::ConnectParams m_connectParams;
    QString m_connectionName;   // QSqlDatabase 连接名
    QString m_currentDatabase;
    QString m_serverVersion;    // 连接时探测的服务器版本（Oracle 11g/12c 语法分派用）
    QString m_oracleConnDisplay; // 已废弃（改用 connDisplay 统一格式）
    bool m_running = false;
    bool m_connected = false;
    bool m_inTransaction = false;  // 处于未提交的手动事务中
    QSqlQuery* m_currentQuery = nullptr;     // 当前正在执行的查询（用于取消）
    bool m_cancelRequested = false;          // 取消标志

    QMutex m_mutex;
    QQueue<Command> m_queue;
};
