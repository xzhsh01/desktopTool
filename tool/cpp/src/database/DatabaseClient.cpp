#include "database/DatabaseClient.h"
#include "database/DriverInstaller.h"
#include "core/Logger.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QSqlField>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QSet>
#include <QRegularExpression>
#include <QAtomicInteger>
#include <QSettings>

// 注册元类型
static const bool regDbTypes = []() {
    qRegisterMetaType<DatabaseClient::QueryResult>("DatabaseClient::QueryResult");
    qRegisterMetaType<DatabaseClient::TableInfo>("DatabaseClient::TableInfo");
    qRegisterMetaType<QList<DatabaseClient::TableInfo>>("QList<DatabaseClient::TableInfo>");
    return true;
}();

// ── DatabaseClient（主线程侧） ────────────────────────────────────────────────

DatabaseClient::DatabaseClient(QObject* parent) : QObject(parent) {}

DatabaseClient::~DatabaseClient() {
    // 同步等待 worker 线程退出，避免本对象销毁后 lambda 仍在访问 m_worker
    if (m_worker) {
        m_worker->disconnect();  // 断开本对象作为接收者的所有连接（包括 finished lambda）
        disconnect();             // 真正停止 worker 线程
        m_worker->deleteLater();
        m_worker = nullptr;
    }
}

QStringList DatabaseClient::availableDrivers() {
    return QSqlDatabase::drivers();
}

bool DatabaseClient::isDbTypeSupported(const QString& dbType) {
    QStringList drivers = QSqlDatabase::drivers();
    if (dbType == "mysql") return drivers.contains("QMYSQL") || drivers.contains("QODBC");
    if (dbType == "postgres") return drivers.contains("QPSQL");
    if (dbType == "sqlite") return drivers.contains("QSQLITE");
    if (dbType == "mssql") return drivers.contains("QODBC");
    if (dbType == "oracle") return drivers.contains("QOCI") || drivers.contains("QODBC");
    return false;
}

void DatabaseClient::connectTo(const ConnectParams& params) {
    if (m_worker && m_worker->isRunning()) {
        emit connectionError("已有数据库连接，请先断开");
        return;
    }
    if (m_worker) {
        // 上一次的 worker 可能处于 Finished 但未释放，确保先彻底释放
        if (m_worker->isRunning()) m_worker->wait(3000);
        m_worker->deleteLater();
        m_worker = nullptr;
    }

    m_worker = new DatabaseWorker(this);
    connect(m_worker, &DatabaseWorker::connected, this, [this](const QString& version) {
        setConnected(true);
        emit connected(version);
    });
    connect(m_worker, &DatabaseWorker::disconnected, this, [this]() {
        setConnected(false);
        emit disconnected();
    });
    connect(m_worker, &DatabaseWorker::connectionError, this, &DatabaseClient::connectionError);
    connect(m_worker, &DatabaseWorker::queryStarted, this, &DatabaseClient::queryStarted);
    connect(m_worker, &DatabaseWorker::queryResult, this, &DatabaseClient::queryResult);
    connect(m_worker, &DatabaseWorker::databasesListed, this, &DatabaseClient::databasesListed);
    connect(m_worker, &DatabaseWorker::tablesListed, this, &DatabaseClient::tablesListed);
    connect(m_worker, &DatabaseWorker::tableDescribed, this, &DatabaseClient::tableDescribed);
    connect(m_worker, &DatabaseWorker::statusMessage, this, &DatabaseClient::statusMessage);
    connect(m_worker, &DatabaseWorker::exportColumnsReady, this, &DatabaseClient::exportColumnsReady);
    connect(m_worker, &DatabaseWorker::exportChunk, this, &DatabaseClient::exportChunk);
    connect(m_worker, &DatabaseWorker::exportFinished, this, &DatabaseClient::exportFinished);
    connect(m_worker, &DatabaseWorker::scriptBatchDone, this, &DatabaseClient::scriptBatchDone);
    connect(m_worker, &DatabaseWorker::objectSqlFetched, this, &DatabaseClient::objectSqlFetched);
    connect(m_worker, &DatabaseWorker::finished, this, [this]() {
        setConnected(false);
        m_worker->deleteLater();
        m_worker = nullptr;
    });

    m_worker->setConnectParams(params);
    m_worker->queueCommand(DatabaseWorker::CmdConnect);
    m_worker->start();
}

void DatabaseClient::disconnect() {
    if (!m_worker) return;
    DatabaseWorker* w = m_worker;
    // 同步等待线程真正结束，避免调用方销毁自己时线程仍在访问成员
    w->queueCommand(DatabaseWorker::CmdDisconnect);
    w->requestStop();
    if (w->isRunning()) {
        w->wait(5000);
    }
}

void DatabaseClient::executeQuery(const QString& query) {
    if (m_worker) m_worker->queueCommand(DatabaseWorker::CmdQuery, query);
}

void DatabaseClient::cancelQuery() {
    if (m_worker) m_worker->requestCancel();
}

void DatabaseClient::listDatabases() {
    if (m_worker) m_worker->queueCommand(DatabaseWorker::CmdListDatabases);
}

void DatabaseClient::listTables(const QString& database) {
    if (m_worker) m_worker->queueCommand(DatabaseWorker::CmdListTables, database);
}

void DatabaseClient::useDatabase(const QString& name) {
    if (m_worker) m_worker->queueCommand(DatabaseWorker::CmdUseDatabase, name);
}

void DatabaseClient::describeTable(const QString& table) {
    if (m_worker) m_worker->queueCommand(DatabaseWorker::CmdDescribeTable, table);
}

void DatabaseClient::selectTableData(const QString& table, int limit, int offset) {
    if (m_worker) {
        // limit 和 offset 编码进 n：n = limit * 1000000 + offset
        m_worker->queueCommand(DatabaseWorker::CmdSelectData, table, limit * 1000000 + offset);
    }
}

void DatabaseClient::getServerVersion() {
    if (m_worker) m_worker->queueCommand(DatabaseWorker::CmdVersion);
}

void DatabaseClient::exportTableData(const QString& table) {
    if (m_worker) m_worker->queueCommand(DatabaseWorker::CmdExportData, table);
}

void DatabaseClient::executeScriptBatch(const QString& sql, int batchIndex, int totalBatches) {
    if (m_worker) m_worker->queueCommand(DatabaseWorker::CmdScriptBatch, sql, batchIndex, totalBatches);
}

void DatabaseClient::fetchObjectSql(const QString& objectType, const QString& object) {
    if (m_worker) m_worker->queueCommand(DatabaseWorker::CmdFetchObjectSql, objectType, object);
}

// ── DatabaseWorker（工作线程侧） ──────────────────────────────────────────────

DatabaseWorker::DatabaseWorker(QObject* parent) : QThread(parent) {
    // 每个客户端独立连接名
    static QAtomicInt counter = 1;
    m_connectionName = QString("dbconn_%1").arg(counter.fetchAndAddRelaxed(1));
}

DatabaseWorker::~DatabaseWorker() {
    requestStop();
    // 等待线程真正退出。disconnect() 已经同步等过，这里再等一次只是兜底
    if (isRunning()) wait(5000);
}

void DatabaseWorker::queueCommand(int type, const QString& a, int n, int n2) {
    QMutexLocker locker(&m_mutex);
    Command cmd{type, a, QString(), n, n2};
    m_queue.enqueue(cmd);
}

void DatabaseWorker::queueCommand(int type, const QString& a, const QString& b) {
    QMutexLocker locker(&m_mutex);
    Command cmd{type, a, b, 0, 0};
    m_queue.enqueue(cmd);
}

void DatabaseWorker::requestStop() {
    m_running = false;
}

void DatabaseWorker::requestCancel() {
    // 通知 doQuery 提前退出（行循环中检查；最坏情况等到 exec 完成）
    m_cancelRequested = true;
    // 尝试中止阻塞中的 exec（部分驱动支持：PG/Odbc 可中断；MySQL/SQLite 多数会忽略）
    if (m_currentQuery) {
        m_currentQuery->finish();
    }
}

void DatabaseWorker::run() {
    m_running = true;

    while (m_running) {
        QQueue<Command> pending;
        {
            QMutexLocker locker(&m_mutex);
            pending.swap(m_queue);
        }

        while (!pending.isEmpty()) {
            Command cmd = pending.dequeue();
            switch (cmd.type) {
                case CmdConnect:
                    if (!doConnect(m_connectParams)) m_running = false;
                    break;
                case CmdDisconnect:
                    doDisconnect();
                    m_running = false;
                    break;
                case CmdQuery:         doQuery(cmd.a); break;
                case CmdListDatabases: doListDatabases(); break;
                case CmdListTables:    doListTables(); break;
                case CmdUseDatabase:   doUseDatabase(cmd.a); break;
                case CmdDescribeTable: doDescribeTable(cmd.a); break;
                case CmdSelectData:    doSelectData(cmd.a, cmd.n / 1000000, cmd.n % 1000000); break;
                case CmdVersion:       doVersion(); break;
                case CmdExportData:    doExportData(cmd.a); break;
                case CmdScriptBatch:   doScriptBatch(cmd.a, cmd.n, cmd.n2); break;
                case CmdFetchObjectSql: doFetchObjectSql(cmd.a, cmd.b); break;
            }
        }

        msleep(m_connected ? 50 : 100);
    }

    doDisconnect();
    emit disconnected();
}

QString DatabaseWorker::driverForType(const QString& dbType) {
    if (dbType == "mysql") {
        // QMYSQL 插件缺失（Qt 官方 MinGW 包不含）时回退 QODBC（MySQL Connector/ODBC）
        QStringList drivers = QSqlDatabase::drivers();
        if (drivers.contains("QMYSQL")) return "QMYSQL";
        return "QODBC";
    }
    if (dbType == "postgres") return "QPSQL";
    if (dbType == "sqlite") return "QSQLITE";
    if (dbType == "mssql") return "QODBC";
    if (dbType == "oracle") {
        QStringList drivers = QSqlDatabase::drivers();
        if (drivers.contains("QOCI")) return "QOCI";
        return "QODBC";
    }
    return {};
}

QString DatabaseWorker::findOdbcDriver(const QString& keyword, const QString& preferred) {
    return DriverInstaller::findOdbcDriver(keyword, preferred);
}

// 若连接串缺少 Uid/Pwd，补上用户名密码
static QString withCredentials(QString connStr, const QString& user, const QString& pass) {
    if (!connStr.contains("Uid=", Qt::CaseInsensitive) && !user.isEmpty()) {
        if (!connStr.endsWith(';')) connStr += ';';
        connStr += "Uid=" + user + ";";
    }
    if (!connStr.contains("Pwd=", Qt::CaseInsensitive)
        && !connStr.contains("Password=", Qt::CaseInsensitive) && !pass.isEmpty()) {
        if (!connStr.endsWith(';')) connStr += ';';
        connStr += "Pwd=" + pass + ";";
    }
    return connStr;
}

QString DatabaseWorker::odbcConnectionString(const DatabaseClient::ConnectParams& params) {
    // 用户提供了完整连接串：直接使用（补全凭据）
    if (!params.connectionString.isEmpty()) {
        return withCredentials(params.connectionString, params.username, params.password);
    }

    // SQL Server / Oracle / MySQL ODBC 连接字符串
    if (params.dbType == "mssql") {
        // 优先新版 "ODBC Driver xx for SQL Server"，回退任意含 sql server 的驱动
        QString drv = findOdbcDriver("sql server", "odbc driver");
        if (drv.isEmpty()) return {};
        return QString("Driver={%1};Server=%2,%3;Database=%4;Uid=%5;Pwd=%6;")
            .arg(drv).arg(params.host).arg(params.port)
            .arg(params.database, params.username, params.password);
    }
    if (params.dbType == "mysql") {
        // QMYSQL 插件缺失时走 MySQL Connector/ODBC（优先 Unicode 驱动）
        QString drv = findOdbcDriver("mysql odbc", "unicode");
        if (drv.isEmpty()) return {};
        return QString("Driver={%1};Server=%2;Port=%3;Database=%4;UID=%5;PWD=%6;")
            .arg(drv).arg(params.host).arg(params.port)
            .arg(params.database, params.username, params.password);
    }
    if (params.dbType == "oracle") {
        // 枚举已安装的 Oracle ODBC 驱动（instantclient / OraClient 等）
        QString drv = findOdbcDriver("oracle", "instantclient");
        if (drv.isEmpty()) return {};
        return QString("Driver={%1};Dbq=%2:%3/%4;Uid=%5;Pwd=%6;")
            .arg(drv).arg(params.host).arg(params.port)
            .arg(params.database, params.username, params.password);
    }
    return {};
}

bool DatabaseWorker::doConnect(const DatabaseClient::ConnectParams& params) {
    emit statusMessage(QString("正在连接 %1...").arg(params.dbType));

    QString driver = driverForType(params.dbType);
    if (driver.isEmpty()) {
        emit connectionError(QString("不支持的数据库类型: %1").arg(params.dbType));
        return false;
    }

    // 检查驱动可用性
    if (!QSqlDatabase::drivers().contains(driver)) {
        emit connectionError(QString("Qt SQL 驱动 %1 不可用。已编译的驱动: %2")
            .arg(driver, QSqlDatabase::drivers().join(", ")));
        return false;
    }

    // SQLite 特殊处理：检查文件存在
    if (params.dbType == "sqlite") {
        if (params.database.isEmpty() || !QFile::exists(params.database)) {
            emit connectionError(QString("SQLite 文件不存在: %1").arg(params.database));
            return false;
        }
    }

    // 创建连接
    QSqlDatabase db;
    if (QSqlDatabase::contains(m_connectionName)) {
        db = QSqlDatabase::database(m_connectionName);
        if (db.isOpen()) db.close();
    } else {
        db = QSqlDatabase::addDatabase(driver, m_connectionName);
    }

    // drivers() 只读插件元数据，addDatabase 才真正加载插件 DLL：
    // 原生驱动加载失败（如 QOCI 缺 oci.dll）时回退 ODBC，避免笼统的 "Driver not loaded"
    if (!db.isValid()) {
        const QString failed = driver;
        QString fallback;
        if (params.dbType == "oracle" || params.dbType == "mysql") fallback = "QODBC";
        if (!fallback.isEmpty() && fallback != failed) {
            Logger::instance().warn(QString("Qt SQL 驱动 %1 加载失败，回退 %2").arg(failed, fallback),
                                    "database");
            QSqlDatabase::removeDatabase(m_connectionName);
            driver = fallback;
            db = QSqlDatabase::addDatabase(driver, m_connectionName);
        }
    }
    if (!db.isValid()) {
        emit connectionError(QString("Qt SQL 驱动 %1 加载失败：插件依赖的运行库缺失。\n"
            "Oracle 需配置 Instant Client 目录到 PATH（设置页可自动安装）；\n"
            "PostgreSQL 需 libpq 运行库；MySQL 建议安装 MySQL ODBC 驱动后走 ODBC。")
            .arg(driver));
        return false;
    }

    if (driver == "QODBC") {
        QString connStr = odbcConnectionString(params);
        if (connStr.isEmpty()) {
            QString hint = (params.dbType == "oracle")
                ? "未在系统中找到 Oracle ODBC 驱动。\n"
                  "请安装 Oracle Instant Client（含 ODBC 组件），"
                  "或安装完整的 Oracle 客户端后重试。"
                : QString("未在系统中找到 %1 的 ODBC 驱动。").arg(params.dbType);
            emit connectionError(hint);
            return false;
        }
        db.setDatabaseName(connStr);
    } else {
        db.setHostName(params.host);
        db.setPort(params.port);
        db.setUserName(params.username);
        db.setPassword(params.password);
        db.setDatabaseName(params.database);
    }
    db.setConnectOptions(QString("CONNECT_TIMEOUT=%1").arg(params.timeoutSec));

    if (!db.open()) {
        emit connectionError(QString("连接失败: %1").arg(db.lastError().text()));
        return false;
    }

    m_connected = true;
    m_currentDatabase = params.database;

    // MySQL / PostgreSQL：连接配置了 database 时，显式 USE / SET search_path，
    // 后续 listDatabases / USE 切换的对象枚举都限定在该库里。
    if (!params.database.isEmpty()) {
        if (params.dbType == "mysql") {
            QString dbName = params.database;
            dbName.replace('`', QStringLiteral("``"));
            QSqlQuery useQ(db);
            if (!useQ.exec(QString("USE `%1`").arg(dbName))) {
                Logger::instance().warn(QString("USE 失败: %1").arg(useQ.lastError().text()), "database");
            }
        } else if (params.dbType == "postgres") {
            QString dbName = params.database;
            dbName.replace('"', QStringLiteral("\"\""));
            QSqlQuery useQ(db);
            if (!useQ.exec(QString("SET search_path TO \"%1\", public").arg(dbName))) {
                Logger::instance().warn(QString("SET search_path 失败: %1").arg(useQ.lastError().text()), "database");
            }
        }
    }

    // Oracle：对象按当前登录用户（schema）组织
    if (params.dbType == "oracle") {
        QSqlQuery uq(db);
        if (uq.exec("SELECT USER FROM DUAL") && uq.next()) {
            m_currentDatabase = uq.value(0).toString();  // 对象枚举用登录用户
        }
    }

    // 获取服务器版本
    QString version;
    QSqlQuery q(db);
    if (params.dbType == "mysql" && q.exec("SELECT VERSION()") && q.next()) {
        version = QString("MySQL %1").arg(q.value(0).toString());
    } else if (params.dbType == "postgres" && q.exec("SELECT version()") && q.next()) {
        version = q.value(0).toString();
    } else if (params.dbType == "sqlite") {
        version = QString("SQLite (%1)").arg(QFileInfo(params.database).fileName());
    } else if (params.dbType == "oracle" && q.exec("SELECT BANNER FROM V$VERSION WHERE BANNER LIKE 'Oracle%'") && q.next()) {
        version = q.value(0).toString();
    } else {
        version = params.dbType;
    }
    m_serverVersion = version;

    Logger::instance().info(QString("数据库已连接: %1 %2:%3/%4")
        .arg(params.dbType, params.host).arg(params.port).arg(params.database), "database");
    emit statusMessage("已连接");
    emit connected(version);
    return true;
}

void DatabaseWorker::doDisconnect() {
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            auto db = QSqlDatabase::database(m_connectionName);
            if (db.isOpen()) {
                if (m_inTransaction) {
                    db.rollback();  // 未提交事务回滚
                    m_inTransaction = false;
                    Logger::instance().info("断开连接：未提交事务已回滚", "database");
                }
                db.close();
            }
        }
        // 释放连接名，避免连接池堆积导致新建连接时取到陈旧句柄
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    m_connected = false;
}

// 提取语句首个关键字（跳过注释与括号前缀）
static QString firstKeyword(const QString& query) {
    QString s = query.trimmed();
    while (s.startsWith("--") || s.startsWith("#")) {
        s = s.section('\n', 1).trimmed();
    }
    if (s.startsWith("/*")) {
        const int end = s.indexOf("*/");
        if (end >= 0) s = s.mid(end + 2).trimmed();
    }
    while (s.startsWith('(')) s = s.mid(1).trimmed();
    return s.section(QRegularExpression("\\s+"), 0, 0).toUpper();
}

void DatabaseWorker::doQuery(const QString& query) {
    emit queryStarted();
    DatabaseClient::QueryResult result;
    QElapsedTimer timer;
    timer.start();

    // 复位取消标志（每次新查询开始时清零）
    m_cancelRequested = false;
    m_currentQuery = nullptr;

    if (!QSqlDatabase::contains(m_connectionName)) {
        result.error = "未连接";
        emit queryResult(query, result);
        return;
    }
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) {
        result.error = "连接已关闭";
        emit queryResult(query, result);
        return;
    }

    // 事务控制语句：手动管理提交（Oracle 的 BEGIN 是 PL/SQL 块，不在此列）
    const QString kw = firstKeyword(query);
    const bool isOracle = (m_connectParams.dbType == "oracle");

    if (kw == "COMMIT") {
        if (!m_inTransaction || db.commit()) {
            m_inTransaction = false;
            result.success = true;
            result.message = "事务已提交";
            result.elapsedMs = timer.elapsed();
            emit queryResult(query, result);
            return;
        }
        // 回退为普通执行（驱动可能不支持）
    } else if (kw == "ROLLBACK") {
        if (!m_inTransaction || db.rollback()) {
            m_inTransaction = false;
            result.success = true;
            result.message = "事务已回滚";
            result.elapsedMs = timer.elapsed();
            emit queryResult(query, result);
            return;
        }
    } else if ((kw == "START" || kw == "BEGIN") && !isOracle) {
        // START TRANSACTION / BEGIN [WORK|TRANSACTION]
        if (db.transaction()) {
            m_inTransaction = true;
            result.success = true;
            result.message = "事务已开启，执行 COMMIT 提交 / ROLLBACK 回滚";
            result.inTransaction = true;
            result.elapsedMs = timer.elapsed();
            emit queryResult(query, result);
            return;
        }
        // 回退为普通执行
    }

    // DML 不自动提交：未在事务中时自动开启事务，用户必须显式 COMMIT/ROLLBACK 才会落库
    // Oracle 的 DML 本身即隐式开启事务，无需特殊处理
    static const QSet<QString> kDmlKeywords = {
        "INSERT", "UPDATE", "DELETE", "MERGE", "REPLACE", "TRUNCATE"
    };
    if (!isOracle && kDmlKeywords.contains(kw) && !m_inTransaction) {
        if (!db.transaction()) {
            result.error = "自动开启事务失败: " + db.lastError().text();
            result.elapsedMs = timer.elapsed();
            Logger::instance().error(result.error, "database");
            emit queryResult(query, result);
            return;
        }
        m_inTransaction = true;
        Logger::instance().info("自动开启事务（DML 不自动提交，需显式 COMMIT）", "database");
    }

    QSqlQuery q(db);
    m_currentQuery = &q;  // 允许 cancelQuery() 调 q.finish() 中断 exec

    // Oracle + Qt QOCI/QODBC：驱动在 exec 时会自动在 SQL 末尾附加分号，
    // Oracle 服务端不接受末尾 ; → ORA-00911 无效字符。统一在 exec 前去除。
    // 注意：splitSqlStatements 已经把用户输入的 ; 当作分隔符，剩余 SQL 末尾不会
    // 出现 ;；但 wrapPagedSql/SELECT USER 等内部 SQL 偶有残留，此处统一防御。
    QString execSql = query;
    if (m_connectParams.dbType == "oracle") {
        while (!execSql.isEmpty()) {
            const QChar c = execSql.at(execSql.size() - 1);
            if (c == ';' || c.isSpace()) execSql.chop(1);
            else break;
        }
    }

    bool ok = q.exec(execSql);
    m_currentQuery = nullptr;
    result.elapsedMs = timer.elapsed();

    // 用户在 exec 期间点了「停止」
    if (m_cancelRequested) {
        result.cancelled = true;
        result.error = "查询已被用户取消";
        // 部分驱动 finish() 后连接不可继续使用，重连以保证后续 SQL 可执行
        if (QSqlDatabase::contains(m_connectionName)) {
            QSqlDatabase::database(m_connectionName).close();
        }
        emit queryResult(query, result);
        return;
    }

    if (!ok) {
        result.error = q.lastError().text();
        // 日志附带 SQL 文本（截断 500 字符），便于定位 ORA-00911 等字符/语法错误
        QString sqlDump = query;
        if (sqlDump.size() > 500) sqlDump = sqlDump.left(500) + QStringLiteral("…(截断)");
        Logger::instance().error(QString("SQL 错误: %1\n失败 SQL: %2")
                                     .arg(result.error, sqlDump), "database");
        emit queryResult(query, result);
        return;
    }

    result.success = true;
    result.inTransaction = m_inTransaction;  // 事务中的 DML 不自动提交

    if (q.isSelect()) {
        // SELECT：收集列名和行（行循环中检查取消，避免大结果集无法中止）
        QSqlRecord rec = q.record();
        for (int i = 0; i < rec.count(); ++i) {
            result.columns << rec.fieldName(i);
        }
        while (q.next()) {
            if (m_cancelRequested) {
                result.cancelled = true;
                result.success = false;
                result.error = QString("查询已取消（已获取 %1 行）").arg(result.rows.size());
                break;
            }
            QVariantList row;
            for (int i = 0; i < rec.count(); ++i) {
                row.append(q.value(i));
            }
            result.rows.append(row);
        }
    } else {
        result.affectedRows = q.numRowsAffected();
    }

    emit queryResult(query, result);
}

void DatabaseWorker::doListDatabases() {
    if (!QSqlDatabase::contains(m_connectionName)) return;
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return;

    QStringList databases;     // 原始库名（用于切换库）
    QStringList displays;      // 显示用：多库类型仅显示库名；Oracle 显示 userName@host:port/service_name
    QSqlQuery q(db);
    const QString dbType = m_connectParams.dbType;

    auto addDb = [&](const QString& rawName) {
        databases << rawName;
        // 树根统一格式：userName@host:port/database；SQLite 没有 host/概念，仅显示文件名。
        if (dbType == "sqlite") {
            displays << rawName;
        } else {
            displays << connDisplay(rawName);
        }
    };

    if (dbType == "mysql") {
        // 连接配置了 database 时，仅显示该库，避免列出一堆系统库（information_schema/performance_schema/sys 等）
        if (!m_connectParams.database.isEmpty()) {
            addDb(m_connectParams.database);
        } else if (q.exec("SHOW DATABASES")) {
            while (q.next()) addDb(q.value(0).toString());
        }
    } else if (dbType == "postgres") {
        // 连接配置了 database 时，仅显示该库（doConnect 已 SET search_path 限定）
        if (!m_connectParams.database.isEmpty()) {
            addDb(m_connectParams.database);
        } else if (q.exec("SELECT datname FROM pg_database WHERE NOT datistemplate")) {
            while (q.next()) addDb(q.value(0).toString());
        }
    } else if (dbType == "sqlite") {
        // 单文件库：树根显示文件名
        addDb(QFileInfo(m_currentDatabase).fileName());
    } else if (dbType == "mssql") {
        if (q.exec("SELECT name FROM sys.databases")) {
            while (q.next()) addDb(q.value(0).toString());
        }
    } else if (dbType == "oracle") {
        // Oracle 顶层节点显示 userName@host:port/service_name（已 login 后的当前 schema）
        addDb(m_currentDatabase);
    }

    // 当前库标识：与 displays 格式一致（SQLite 仅文件名，其它走 connDisplay）
    const QString currentDisplay = (dbType == "sqlite")
        ? m_currentDatabase
        : connDisplay(m_currentDatabase);
    emit databasesListed(displays, currentDisplay);
}

// 顶层节点显示格式：userName@host:port/database
QString DatabaseWorker::connDisplay(const QString& dbName) const {
    return QString("%1@%2:%3/%4")
        .arg(m_connectParams.username, m_connectParams.host)
        .arg(m_connectParams.port).arg(dbName);
}

void DatabaseWorker::doListTables() {
    if (!QSqlDatabase::contains(m_connectionName)) return;
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return;

    QList<DatabaseClient::TableInfo> objects;
    const QString dbType = m_connectParams.dbType;
    const QString curDb = QString(m_currentDatabase).replace("'", "''");

    // 通用辅助：执行「名称 + 类型」两列查询并按映射归一化类型
    auto addObjects = [&](const QString& sql, const QMap<QString, QString>& typeMap) {
        QSqlQuery q(db);
        q.setForwardOnly(true);  // ODBC 下前向游标，大批量行拉取更快
        if (!q.exec(sql)) return;
        while (q.next()) {
            DatabaseClient::TableInfo info;
            info.name = q.value(0).toString();
            const QString raw = q.value(1).toString().toUpper();
            info.type = typeMap.value(raw, "table");
            objects.append(info);
        }
    };
    // 三列版（schema + 名称 + 类型）：PostgreSQL 按 schema 组织对象树
    auto addObjectsWithSchema = [&](const QString& sql, const QMap<QString, QString>& typeMap) {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        if (!q.exec(sql)) return;
        while (q.next()) {
            DatabaseClient::TableInfo info;
            info.schema = q.value(0).toString();
            info.name = q.value(1).toString();
            const QString raw = q.value(2).toString().toUpper();
            info.type = typeMap.value(raw, "table");
            objects.append(info);
        }
    };

    if (dbType == "mysql") {
        addObjects(QString("SELECT TABLE_NAME, TABLE_TYPE FROM information_schema.TABLES "
                           "WHERE TABLE_SCHEMA = '%1'").arg(curDb),
                   {{"BASE TABLE", "table"}, {"VIEW", "view"}, {"SYSTEM VIEW", "view"}});
        addObjects(QString("SELECT ROUTINE_NAME, ROUTINE_TYPE FROM information_schema.ROUTINES "
                           "WHERE ROUTINE_SCHEMA = '%1'").arg(curDb),
                   {{"PROCEDURE", "procedure"}, {"FUNCTION", "function"}});
        addObjects(QString("SELECT TRIGGER_NAME, 'TRIGGER' FROM information_schema.TRIGGERS "
                           "WHERE TRIGGER_SCHEMA = '%1'").arg(curDb),
                   {{"TRIGGER", "trigger"}});
        addObjects(QString("SELECT EVENT_NAME, 'EVENT' FROM information_schema.EVENTS "
                           "WHERE EVENT_SCHEMA = '%1'").arg(curDb),
                   {{"EVENT", "job"}});
    } else if (dbType == "oracle") {
        // 按当前登录用户（schema）枚举全部对象类型。
        // 性能关键点：
        //  1) 当前 schema 即登录用户时用 USER_* 视图 —— 只扫当前用户对象，
        //     避免 ALL_OBJECTS 全字典扫描 + OWNER 过滤（大库上慢一个数量级）；
        //  2) 去掉 ORDER BY —— UI 侧按类型分组渲染，排序纯属浪费；
        //  3) 排除回收站对象（BIN$ 前缀）。
        const QString kOracleTypes =
            "('TABLE','VIEW','MATERIALIZED VIEW','SEQUENCE','SYNONYM','TYPE',"
            "'PROCEDURE','FUNCTION','PACKAGE','PACKAGE BODY','TRIGGER','DATABASE LINK','INDEX')";
        const QMap<QString, QString> kOracleMap = {
            {"TABLE", "table"}, {"VIEW", "view"}, {"MATERIALIZED VIEW", "matview"},
            {"SEQUENCE", "sequence"}, {"SYNONYM", "synonym"}, {"TYPE", "type"},
            {"PROCEDURE", "procedure"}, {"FUNCTION", "function"}, {"PACKAGE", "package"},
            {"PACKAGE BODY", "packagebody"}, {"TRIGGER", "trigger"},
            {"DATABASE LINK", "dblink"}, {"INDEX", "index"}};

        // 确认当前 schema 是否就是登录用户（正常情况恒为真；切换 schema 被屏蔽）
        QString oracleUser;
        {
            QSqlQuery uq(db);
            if (uq.exec("SELECT USER FROM DUAL") && uq.next())
                oracleUser = uq.value(0).toString();
        }
        const bool ownSchema =
            oracleUser.isEmpty() || oracleUser.compare(curDb, Qt::CaseInsensitive) == 0;

        if (ownSchema) {
            addObjects(QString("SELECT OBJECT_NAME, OBJECT_TYPE FROM USER_OBJECTS "
                               "WHERE OBJECT_TYPE IN %1 "
                               "AND OBJECT_NAME NOT LIKE 'BIN$%'").arg(kOracleTypes),
                       kOracleMap);
            addObjects("SELECT JOB_NAME, 'JOB' FROM USER_SCHEDULER_JOBS",
                       {{"JOB", "job"}});
        } else {
            // 兜底：浏览非登录 schema 时才走 ALL_OBJECTS
            addObjects(QString("SELECT OBJECT_NAME, OBJECT_TYPE FROM ALL_OBJECTS "
                               "WHERE OWNER = '%1' AND OBJECT_TYPE IN %2 "
                               "AND OBJECT_NAME NOT LIKE 'BIN$%'")
                           .arg(curDb, kOracleTypes),
                       kOracleMap);
            addObjects(QString("SELECT JOB_NAME, 'JOB' FROM ALL_SCHEDULER_JOBS WHERE OWNER = '%1'")
                           .arg(curDb),
                       {{"JOB", "job"}});
        }
    } else if (dbType == "mssql") {
        addObjects("SELECT name, type FROM sys.objects "
                   "WHERE type IN ('U','V','P','FN','IF','TF','TR','SO') AND is_ms_shipped = 0 "
                   "ORDER BY type, name",
                   {{"U", "table"}, {"V", "view"}, {"P", "procedure"}, {"FN", "function"},
                    {"IF", "function"}, {"TF", "function"}, {"TR", "trigger"},
                    {"SO", "sequence"}});
        // 用户自定义类型（表类型/别名类型等）
        addObjects("SELECT name, 'TYPE' FROM sys.types WHERE is_user_defined = 1",
                   {{"TYPE", "type"}});
        addObjects("SELECT name, 'JOB' FROM msdb.dbo.sysjobs", {{"JOB", "job"}});
    } else if (dbType == "sqlite") {
        addObjects("SELECT name, type FROM sqlite_master "
                   "WHERE type IN ('table','view','trigger','index') AND name NOT LIKE 'sqlite_%' "
                   "ORDER BY type, name",
                   {{"TABLE", "table"}, {"VIEW", "view"}, {"TRIGGER", "trigger"},
                    {"INDEX", "index"}});
    } else if (dbType == "postgres") {
        // 表/视图（全部用户 schema，排除系统 schema）
        addObjectsWithSchema(
            "SELECT table_schema, table_name, table_type FROM information_schema.tables "
            "WHERE table_schema NOT IN ('pg_catalog','information_schema') "
            "AND table_schema NOT LIKE 'pg_toast%' "
            "ORDER BY table_schema, table_name",
            {{"BASE TABLE", "table"}, {"VIEW", "view"}});
        // 物化视图
        addObjectsWithSchema(
            "SELECT n.nspname, c.relname, 'MATERIALIZED VIEW' FROM pg_class c "
            "JOIN pg_namespace n ON n.oid = c.relnamespace "
            "WHERE c.relkind = 'm' AND n.nspname NOT IN ('pg_catalog','information_schema')",
            {{"MATERIALIZED VIEW", "matview"}});
        // 序列
        addObjectsWithSchema(
            "SELECT n.nspname, c.relname, 'SEQUENCE' FROM pg_class c "
            "JOIN pg_namespace n ON n.oid = c.relnamespace "
            "WHERE c.relkind = 'S' AND n.nspname NOT IN ('pg_catalog','information_schema')",
            {{"SEQUENCE", "sequence"}});
        // 函数/存储过程（prokind 为 PG11+；失败则跳过）
        addObjectsWithSchema(
            "SELECT n.nspname, p.proname, CASE p.prokind WHEN 'p' THEN 'PROCEDURE' ELSE 'FUNCTION' END "
            "FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
            "WHERE n.nspname NOT IN ('pg_catalog','information_schema')",
            {{"FUNCTION", "function"}, {"PROCEDURE", "procedure"}});
        // 触发器（排除系统内部触发器 tgisinternal）
        addObjectsWithSchema(
            "SELECT n.nspname, t.tgname, 'TRIGGER' FROM pg_trigger t "
            "JOIN pg_class c ON c.oid = t.tgrelid "
            "JOIN pg_namespace n ON n.oid = c.relnamespace "
            "WHERE NOT t.tgisinternal "
            "AND n.nspname NOT IN ('pg_catalog','information_schema')",
            {{"TRIGGER", "trigger"}});
        // 自定义类型：枚举(e)/复合(c)/域(d)，排除表行类型（pg_class.reltype 关联的自动复合类型）
        addObjectsWithSchema(
            "SELECT n.nspname, t.typname, 'TYPE' FROM pg_type t "
            "JOIN pg_namespace n ON n.oid = t.typnamespace "
            "WHERE t.typtype IN ('e','c','d') "
            "AND n.nspname NOT IN ('pg_catalog','information_schema') "
            "AND NOT EXISTS (SELECT 1 FROM pg_class cc WHERE cc.reltype = t.oid)",
            {{"TYPE", "type"}});
        // 索引（普通索引 relkind='i'，排除系统 schema）
        addObjectsWithSchema(
            "SELECT n.nspname, c.relname, 'INDEX' FROM pg_class c "
            "JOIN pg_namespace n ON n.oid = c.relnamespace "
            "WHERE c.relkind = 'i' "
            "AND n.nspname NOT IN ('pg_catalog','information_schema')",
            {{"INDEX", "index"}});
    } else {
        // 回退：Qt 内置表列表
        for (const QString& name : db.tables(QSql::Tables)) {
            DatabaseClient::TableInfo info;
            info.name = name;
            objects.append(info);
        }
    }

    // 行数估算（MySQL，仅表）
    if (dbType == "mysql") {
        QSqlQuery q(db);
        for (auto& info : objects) {
            if (info.type != "table") continue;
            if (q.exec(QString("SELECT TABLE_ROWS FROM information_schema.TABLES "
                               "WHERE TABLE_SCHEMA = '%1' AND TABLE_NAME = '%2'")
                       .arg(curDb, info.name)) && q.next()) {
                info.rowCount = q.value(0).toLongLong();
            }
        }
    }

    std::sort(objects.begin(), objects.end(),
              [](const DatabaseClient::TableInfo& a, const DatabaseClient::TableInfo& b) {
        if (a.type != b.type) return a.type < b.type;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });

    emit tablesListed(objects);
}

void DatabaseWorker::doUseDatabase(const QString& name) {
    if (!QSqlDatabase::contains(m_connectionName)) return;
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen() || name.isEmpty()) return;

    const QString dbType = m_connectParams.dbType;
    const QString esc = QString(name).replace("'", "''");

    if (dbType == "mysql" || dbType == "mssql") {
        QSqlQuery q(db);
        if (!q.exec(QString("USE %1").arg(esc))) {
            emit statusMessage(QString("切换数据库失败: %1").arg(name));
            return;
        }
        m_currentDatabase = name;
        doListTables();
    } else if (dbType == "oracle") {
        // Oracle 顶层节点是连接串，用户切换已屏蔽
        Q_UNUSED(name);
    } else if (dbType == "postgres") {
        // PG 跨库必须重连；connected 信号会触发 UI 重新加载对象树
        DatabaseClient::ConnectParams params = m_connectParams;
        params.database = name;
        doDisconnect();
        doConnect(params);
    }
    // SQLite：单文件库，忽略
}

void DatabaseWorker::doDescribeTable(const QString& table) {
    if (!QSqlDatabase::contains(m_connectionName)) return;
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return;

    DatabaseClient::TableInfo info;
    info.name = table;

    // 拆分 schema.table（右键菜单传入的是限定名）
    QString schemaName = m_currentDatabase;
    QString tbl = table;
    const int dot = table.indexOf('.');
    if (dot > 0) {
        schemaName = table.left(dot);
        tbl = table.mid(dot + 1);
        info.schema = schemaName;
    }
    auto esc = [](QString s) { s.replace('\'', QStringLiteral("''")); return s; };
    const QString schemaEsc = esc(schemaName);
    const QString tblEsc = esc(tbl);
    const QString dbType = m_connectParams.dbType;

    QSqlRecord rec = db.record(table);
    for (int i = 0; i < rec.count(); ++i) {
        QSqlField field = rec.field(i);
        info.columns << field.name();
        // Qt6: QSqlField::metaType() 返回 QMetaType
        info.columnTypes << QString::fromLatin1(field.metaType().name());
    }

    // 主键（MySQL/PG/Oracle/SQLite）
    QSqlQuery q(db);
    if (dbType == "mysql") {
        if (q.exec(QString("SHOW KEYS FROM `%1` WHERE Key_name = 'PRIMARY'").arg(tblEsc))) {
            while (q.next()) {
                info.primaryKeys << q.value("Column_name").toString();
            }
        }
    } else if (dbType == "sqlite") {
        if (q.exec(QString("PRAGMA table_info(\"%1\")").arg(tblEsc))) {
            while (q.next()) {
                if (q.value(5).toInt() > 0) info.primaryKeys << q.value(1).toString();
            }
        }
    } else if (dbType == "postgres") {
        if (q.exec(QString(
                "SELECT a.attname FROM pg_index i "
                "JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = ANY(i.indkey) "
                "WHERE i.indrelid = '%1'::regclass AND i.indisprimary").arg(table))) {
            while (q.next()) {
                info.primaryKeys << q.value(0).toString();
            }
        }
    } else if (dbType == "oracle") {
        // Oracle 主键：当前 schema（登录用户）下该表的主键列，按约束列序输出
        if (q.exec(QString(
                "SELECT cc.COLUMN_NAME FROM ALL_CONSTRAINTS c "
                "JOIN ALL_CONS_COLUMNS cc "
                "  ON c.OWNER = cc.OWNER AND c.CONSTRAINT_NAME = cc.CONSTRAINT_NAME "
                "WHERE c.CONSTRAINT_TYPE = 'P' AND c.OWNER = '%1' AND c.TABLE_NAME = '%2' "
                "ORDER BY cc.POSITION").arg(schemaEsc, tblEsc))) {
            while (q.next()) {
                info.primaryKeys << q.value(0).toString();
            }
        }
    }

    // ── MySQL：字段详情/外键/索引/分区 ──
    if (dbType == "mysql") {
        if (q.exec(QString(
                "SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT, EXTRA, COLUMN_COMMENT "
                "FROM information_schema.COLUMNS "
                "WHERE TABLE_SCHEMA='%1' AND TABLE_NAME='%2' ORDER BY ORDINAL_POSITION")
                .arg(schemaEsc, tblEsc))) {
            while (q.next()) {
                DatabaseClient::ColumnDetail cd;
                cd.name = q.value(0).toString();
                cd.type = q.value(1).toString();
                cd.nullable = (q.value(2).toString().compare("YES", Qt::CaseInsensitive) == 0);
                cd.defaultValue = q.value(3).isNull() ? QString() : q.value(3).toString();
                cd.autoIncrement = q.value(4).toString().contains("auto_increment", Qt::CaseInsensitive);
                cd.comment = q.value(5).toString();
                cd.primaryKey = info.primaryKeys.contains(cd.name);
                info.columnDetails.append(cd);
            }
        }
        if (q.exec(QString(
                "SELECT CONSTRAINT_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME "
                "FROM information_schema.KEY_COLUMN_USAGE "
                "WHERE TABLE_SCHEMA='%1' AND TABLE_NAME='%2' AND REFERENCED_TABLE_NAME IS NOT NULL "
                "ORDER BY CONSTRAINT_NAME, ORDINAL_POSITION").arg(schemaEsc, tblEsc))) {
            while (q.next()) {
                info.foreignKeys.append({q.value(0).toString(), q.value(1).toString(),
                                         q.value(2).toString(), q.value(3).toString()});
            }
        }
        if (q.exec(QString(
                "SELECT INDEX_NAME, NON_UNIQUE, INDEX_TYPE, COLUMN_NAME "
                "FROM information_schema.STATISTICS "
                "WHERE TABLE_SCHEMA='%1' AND TABLE_NAME='%2' ORDER BY INDEX_NAME, SEQ_IN_INDEX")
                .arg(schemaEsc, tblEsc))) {
            while (q.next()) {
                const QString idxName = q.value(0).toString();
                DatabaseClient::IndexInfo* target = nullptr;
                for (auto& ix : info.indexes) if (ix.name == idxName) { target = &ix; break; }
                if (!target) {
                    DatabaseClient::IndexInfo ix;
                    ix.name = idxName;
                    ix.type = (idxName == "PRIMARY") ? "PRIMARY"
                            : (q.value(1).toInt() == 0 ? "UNIQUE" : q.value(2).toString());
                    info.indexes.append(ix);
                    target = &info.indexes.last();
                }
                target->columns << q.value(3).toString();
            }
        }
        if (q.exec(QString(
                "SELECT DISTINCT PARTITION_NAME, PARTITION_METHOD, PARTITION_EXPRESSION "
                "FROM information_schema.PARTITIONS "
                "WHERE TABLE_SCHEMA='%1' AND TABLE_NAME='%2' AND PARTITION_NAME IS NOT NULL "
                "ORDER BY PARTITION_ORDINAL_POSITION").arg(schemaEsc, tblEsc))) {
            while (q.next()) {
                info.partitions.append({q.value(0).toString(), q.value(1).toString(),
                                        q.value(2).toString()});
            }
        }
    }

    // ── PostgreSQL：字段详情/外键/索引/分区 ──
    if (dbType == "postgres") {
        if (q.exec(QString(
                "SELECT c.column_name, "
                "  c.udt_name || COALESCE('(' || c.character_maximum_length || ')', ''), "
                "  c.is_nullable, c.column_default, c.is_identity, pgd.description "
                "FROM information_schema.columns c "
                "LEFT JOIN pg_catalog.pg_statio_all_tables st "
                "  ON st.schemaname = c.table_schema AND st.relname = c.table_name "
                "LEFT JOIN pg_catalog.pg_description pgd "
                "  ON pgd.objoid = st.relid AND pgd.objsubid = c.ordinal_position "
                "WHERE c.table_schema='%1' AND c.table_name='%2' ORDER BY c.ordinal_position")
                .arg(schemaEsc, tblEsc))) {
            while (q.next()) {
                DatabaseClient::ColumnDetail cd;
                cd.name = q.value(0).toString();
                cd.type = q.value(1).toString();
                cd.nullable = (q.value(2).toString().compare("YES", Qt::CaseInsensitive) == 0);
                cd.defaultValue = q.value(3).isNull() ? QString() : q.value(3).toString();
                const bool identity = (q.value(4).toString().compare("YES", Qt::CaseInsensitive) == 0);
                cd.autoIncrement = identity || cd.defaultValue.startsWith("nextval(");
                cd.comment = q.value(5).toString();
                cd.primaryKey = info.primaryKeys.contains(cd.name);
                info.columnDetails.append(cd);
            }
        }
        if (q.exec(QString(
                "SELECT con.conname, att.attname, cl2.relname, att2.attname "
                "FROM pg_constraint con "
                "JOIN pg_class cl ON cl.oid = con.conrelid "
                "JOIN pg_namespace ns ON ns.oid = cl.relnamespace "
                "JOIN pg_attribute att ON att.attrelid = con.conrelid AND att.attnum = con.conkey[1] "
                "JOIN pg_class cl2 ON cl2.oid = con.confrelid "
                "JOIN pg_attribute att2 ON att2.attrelid = con.confrelid AND att2.attnum = con.confkey[1] "
                "WHERE con.contype='f' AND ns.nspname='%1' AND cl.relname='%2'")
                .arg(schemaEsc, tblEsc))) {
            while (q.next()) {
                info.foreignKeys.append({q.value(0).toString(), q.value(1).toString(),
                                         q.value(2).toString(), q.value(3).toString()});
            }
        }
        if (q.exec(QString(
                "SELECT i.relname, ix.indisunique, ix.indisprimary, a.attname "
                "FROM pg_class t "
                "JOIN pg_namespace n ON n.oid = t.relnamespace "
                "JOIN pg_index ix ON t.oid = ix.indrelid "
                "JOIN pg_class i ON i.oid = ix.indexrelid "
                "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = ANY(ix.indkey) "
                "WHERE t.relname='%1' AND n.nspname='%2' "
                "ORDER BY i.relname, array_position(ix.indkey, a.attnum)").arg(tblEsc, schemaEsc))) {
            while (q.next()) {
                const QString idxName = q.value(0).toString();
                DatabaseClient::IndexInfo* target = nullptr;
                for (auto& ix : info.indexes) if (ix.name == idxName) { target = &ix; break; }
                if (!target) {
                    DatabaseClient::IndexInfo ix;
                    ix.name = idxName;
                    ix.type = q.value(2).toBool() ? "PRIMARY" : (q.value(1).toBool() ? "UNIQUE" : "NORMAL");
                    info.indexes.append(ix);
                    target = &info.indexes.last();
                }
                target->columns << q.value(3).toString();
            }
        }
        if (q.exec(QString(
                "SELECT c.relname, p.partstrat, pg_get_partkeydef(p.partrelid) "
                "FROM pg_partitioned_table p "
                "JOIN pg_inherits inh ON inh.inhparent = p.partrelid "
                "JOIN pg_class c ON c.oid = inh.inhrelid "
                "WHERE p.partrelid = '%1'::regclass").arg(table))) {
            while (q.next()) {
                const QString strat = q.value(1).toString();
                const QString ptype = (strat == "r") ? "RANGE" : (strat == "l") ? "LIST" : "HASH";
                info.partitions.append({q.value(0).toString(), ptype, q.value(2).toString()});
            }
        }
    }

    // ── Oracle：字段详情/自增/外键/索引/分区 ──
    if (dbType == "oracle") {
        if (q.exec(QString(
                "SELECT c.COLUMN_NAME, "
                "  c.DATA_TYPE || CASE "
                "    WHEN c.DATA_PRECISION IS NOT NULL THEN '(' || c.DATA_PRECISION || "
                "         NVL2(c.DATA_SCALE, ',' || c.DATA_SCALE, '') || ')' "
                "    WHEN c.CHAR_LENGTH > 0 THEN '(' || c.CHAR_LENGTH || ')' ELSE '' END, "
                "  c.NULLABLE, c.DATA_DEFAULT, m.COMMENTS "
                "FROM ALL_TAB_COLUMNS c "
                "LEFT JOIN ALL_COL_COMMENTS m "
                "  ON m.OWNER = c.OWNER AND m.TABLE_NAME = c.TABLE_NAME AND m.COLUMN_NAME = c.COLUMN_NAME "
                "WHERE c.OWNER = '%1' AND c.TABLE_NAME = '%2' ORDER BY c.COLUMN_ID")
                .arg(schemaEsc, tblEsc))) {
            while (q.next()) {
                DatabaseClient::ColumnDetail cd;
                cd.name = q.value(0).toString();
                cd.type = q.value(1).toString();
                cd.nullable = (q.value(2).toString() == "Y");
                cd.defaultValue = q.value(3).isNull() ? QString() : q.value(3).toString().trimmed();
                cd.comment = q.value(4).toString();
                cd.primaryKey = info.primaryKeys.contains(cd.name);
                info.columnDetails.append(cd);
            }
        }
        // 自增（12c IDENTITY；11g 无此视图，exec 失败则忽略）
        QStringList identityCols;
        if (q.exec(QString(
                "SELECT COLUMN_NAME FROM ALL_TAB_IDENTITY_COLS "
                "WHERE OWNER = '%1' AND TABLE_NAME = '%2'").arg(schemaEsc, tblEsc))) {
            while (q.next()) identityCols << q.value(0).toString();
        }
        for (auto& cd : info.columnDetails)
            cd.autoIncrement = identityCols.contains(cd.name);
        if (q.exec(QString(
                "SELECT c.CONSTRAINT_NAME, cc.COLUMN_NAME, rc.TABLE_NAME, rcc.COLUMN_NAME "
                "FROM ALL_CONSTRAINTS c "
                "JOIN ALL_CONS_COLUMNS cc "
                "  ON cc.OWNER = c.OWNER AND cc.CONSTRAINT_NAME = c.CONSTRAINT_NAME "
                "JOIN ALL_CONSTRAINTS rc "
                "  ON rc.OWNER = c.R_OWNER AND rc.CONSTRAINT_NAME = c.R_CONSTRAINT_NAME "
                "JOIN ALL_CONS_COLUMNS rcc "
                "  ON rcc.OWNER = rc.OWNER AND rcc.CONSTRAINT_NAME = rc.CONSTRAINT_NAME "
                " AND rcc.POSITION = cc.POSITION "
                "WHERE c.CONSTRAINT_TYPE = 'R' AND c.OWNER = '%1' AND c.TABLE_NAME = '%2'")
                .arg(schemaEsc, tblEsc))) {
            while (q.next()) {
                info.foreignKeys.append({q.value(0).toString(), q.value(1).toString(),
                                         q.value(2).toString(), q.value(3).toString()});
            }
        }
        if (q.exec(QString(
                "SELECT i.INDEX_NAME, i.UNIQUENESS, i.INDEX_TYPE, ic.COLUMN_NAME "
                "FROM ALL_INDEXES i "
                "JOIN ALL_IND_COLUMNS ic "
                "  ON ic.INDEX_OWNER = i.OWNER AND ic.INDEX_NAME = i.INDEX_NAME "
                " AND ic.TABLE_OWNER = i.TABLE_OWNER AND ic.TABLE_NAME = i.TABLE_NAME "
                "WHERE i.TABLE_OWNER = '%1' AND i.TABLE_NAME = '%2' "
                "ORDER BY i.INDEX_NAME, ic.COLUMN_POSITION").arg(schemaEsc, tblEsc))) {
            while (q.next()) {
                const QString idxName = q.value(0).toString();
                DatabaseClient::IndexInfo* target = nullptr;
                for (auto& ix : info.indexes) if (ix.name == idxName) { target = &ix; break; }
                if (!target) {
                    DatabaseClient::IndexInfo ix;
                    ix.name = idxName;
                    ix.type = (q.value(1).toString() == "UNIQUE") ? "UNIQUE" : q.value(2).toString();
                    info.indexes.append(ix);
                    target = &info.indexes.last();
                }
                target->columns << q.value(3).toString();
            }
        }
        QString partType, partCol;
        if (q.exec(QString(
                "SELECT t.PARTITIONING_TYPE, kc.NAME FROM ALL_PART_TABLES t "
                "JOIN ALL_PART_KEY_COLUMNS kc ON kc.OWNER = t.OWNER AND kc.NAME = t.TABLE_NAME "
                "WHERE t.OWNER = '%1' AND t.TABLE_NAME = '%2'").arg(schemaEsc, tblEsc))) {
            if (q.next()) { partType = q.value(0).toString(); partCol = q.value(1).toString(); }
        }
        if (!partType.isEmpty()) {
            if (q.exec(QString(
                    "SELECT PARTITION_NAME FROM ALL_TAB_PARTITIONS "
                    "WHERE TABLE_OWNER = '%1' AND TABLE_NAME = '%2' ORDER BY PARTITION_POSITION")
                    .arg(schemaEsc, tblEsc))) {
                while (q.next()) info.partitions.append({q.value(0).toString(), partType, partCol});
            }
        }
    }

    // ── SQLite：字段详情/外键/索引（无注释/分区概念） ──
    if (dbType == "sqlite") {
        QString createSql;
        {
            QSqlQuery q2(db);
            if (q2.exec(QString("SELECT sql FROM sqlite_master WHERE name='%1'").arg(tblEsc)) && q2.next())
                createSql = q2.value(0).toString();
        }
        if (q.exec(QString("PRAGMA table_info(\"%1\")").arg(tblEsc))) {
            while (q.next()) {
                DatabaseClient::ColumnDetail cd;
                cd.name = q.value(1).toString();
                cd.type = q.value(2).toString();
                cd.nullable = (q.value(3).toInt() == 0);
                cd.defaultValue = q.value(4).isNull() ? QString() : q.value(4).toString();
                cd.primaryKey = (q.value(5).toInt() > 0);
                // INTEGER PRIMARY KEY + AUTOINCREMENT 关键字判定
                cd.autoIncrement = cd.primaryKey
                    && cd.type.compare("INTEGER", Qt::CaseInsensitive) == 0
                    && createSql.contains("AUTOINCREMENT", Qt::CaseInsensitive);
                info.columnDetails.append(cd);
            }
        }
        if (q.exec(QString("PRAGMA foreign_key_list(\"%1\")").arg(tblEsc))) {
            QHash<int, DatabaseClient::ForeignKeyInfo> fkById;
            while (q.next()) {
                DatabaseClient::ForeignKeyInfo fk;
                fk.name = QString("fk_%1_%2").arg(tbl, q.value(0).toString());
                fk.column = q.value(3).toString();
                fk.refTable = q.value(2).toString();
                fk.refColumn = q.value(4).toString();
                info.foreignKeys.append(fk);
            }
        }
        QStringList idxNames;
        {
            QSqlQuery q2(db);
            if (q2.exec(QString("PRAGMA index_list(\"%1\")").arg(tblEsc))) {
                while (q2.next()) {
                    DatabaseClient::IndexInfo ix;
                    ix.name = q2.value(1).toString();
                    ix.type = q2.value(2).toInt() != 0 ? "UNIQUE" : "NORMAL";
                    info.indexes.append(ix);
                    idxNames << ix.name;
                }
            }
        }
        for (int i = 0; i < idxNames.size(); ++i) {
            QSqlQuery q3(db);
            if (q3.exec(QString("PRAGMA index_info(\"%1\")").arg(idxNames[i]))) {
                while (q3.next()) info.indexes[i].columns << q3.value(2).toString();
            }
        }
    }

    // ── MSSQL：字段详情/外键/索引 ──
    if (dbType == "mssql") {
        if (q.exec(QString(
                "SELECT c.COLUMN_NAME, "
                "  c.DATA_TYPE + COALESCE('(' + CAST(c.CHARACTER_MAXIMUM_LENGTH AS VARCHAR) + ')', ''), "
                "  c.IS_NULLABLE, c.COLUMN_DEFAULT, "
                "  COLUMNPROPERTY(OBJECT_ID(c.TABLE_SCHEMA + '.' + c.TABLE_NAME), c.COLUMN_NAME, 'IsIdentity'), "
                "  CAST(ep.value AS NVARCHAR(4000)) "
                "FROM INFORMATION_SCHEMA.COLUMNS c "
                "LEFT JOIN sys.columns sc "
                "  ON sc.object_id = OBJECT_ID(c.TABLE_SCHEMA + '.' + c.TABLE_NAME) AND sc.name = c.COLUMN_NAME "
                "LEFT JOIN sys.extended_properties ep "
                "  ON ep.major_id = sc.object_id AND ep.minor_id = sc.column_id AND ep.name = 'MS_Description' "
                "WHERE c.TABLE_NAME='%1' ORDER BY c.ORDINAL_POSITION").arg(tblEsc))) {
            while (q.next()) {
                DatabaseClient::ColumnDetail cd;
                cd.name = q.value(0).toString();
                cd.type = q.value(1).toString();
                cd.nullable = (q.value(2).toString().compare("YES", Qt::CaseInsensitive) == 0);
                cd.defaultValue = q.value(3).isNull() ? QString() : q.value(3).toString();
                cd.autoIncrement = (q.value(4).toInt() == 1);
                cd.comment = q.value(5).toString();
                cd.primaryKey = info.primaryKeys.contains(cd.name);
                info.columnDetails.append(cd);
            }
        }
        if (q.exec(QString(
                "SELECT fk.name, pc.name, rt.name, rc.name "
                "FROM sys.foreign_keys fk "
                "JOIN sys.foreign_key_columns fkc ON fkc.constraint_object_id = fk.object_id "
                "JOIN sys.tables pt ON pt.object_id = fk.parent_object_id "
                "JOIN sys.columns pc ON pc.object_id = pt.object_id AND pc.column_id = fkc.parent_column_id "
                "JOIN sys.tables rt ON rt.object_id = fk.referenced_object_id "
                "JOIN sys.columns rc ON rc.object_id = rt.object_id AND rc.column_id = fkc.referenced_column_id "
                "WHERE pt.name='%1'").arg(tblEsc))) {
            while (q.next()) {
                info.foreignKeys.append({q.value(0).toString(), q.value(1).toString(),
                                         q.value(2).toString(), q.value(3).toString()});
            }
        }
        if (q.exec(QString(
                "SELECT i.name, i.type_desc, i.is_unique, i.is_primary_key, c.name "
                "FROM sys.indexes i "
                "JOIN sys.tables t ON t.object_id = i.object_id "
                "JOIN sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
                "JOIN sys.columns c ON c.object_id = i.object_id AND c.column_id = ic.column_id "
                "WHERE t.name='%1' AND i.name IS NOT NULL ORDER BY i.name, ic.key_ordinal").arg(tblEsc))) {
            while (q.next()) {
                const QString idxName = q.value(0).toString();
                DatabaseClient::IndexInfo* target = nullptr;
                for (auto& ix : info.indexes) if (ix.name == idxName) { target = &ix; break; }
                if (!target) {
                    DatabaseClient::IndexInfo ix;
                    ix.name = idxName;
                    ix.type = q.value(3).toBool() ? "PRIMARY"
                            : (q.value(2).toBool() ? "UNIQUE" : q.value(1).toString());
                    info.indexes.append(ix);
                    target = &info.indexes.last();
                }
                target->columns << q.value(4).toString();
            }
        }
    }

    emit tableDescribed(info);
}

void DatabaseWorker::doSelectData(const QString& table, int limit, int offset) {
    // 转义表名（防注入：仅允许字母数字下划线和点）
    QString safeTable = table;
    safeTable.remove(QRegularExpression("[^a-zA-Z0-9_\\.]"));

    QString query;
    if (m_connectParams.dbType == "mysql") {
        query = QString("SELECT * FROM `%1` LIMIT %2 OFFSET %3").arg(table).arg(limit).arg(offset);
    } else if (m_connectParams.dbType == "postgres" || m_connectParams.dbType == "sqlite") {
        query = QString("SELECT * FROM \"%1\" LIMIT %2 OFFSET %3").arg(safeTable).arg(limit).arg(offset);
    } else if (m_connectParams.dbType == "oracle") {
        // OFFSET/FETCH 是 Oracle 12c+ 语法；11g 及更早版本回退 ROWNUM 包装
        static const QRegularExpression verRe(QStringLiteral("(\\d+)"));
        const int major = verRe.match(m_serverVersion).hasMatch()
            ? verRe.match(m_serverVersion).captured(1).toInt() : 0;
        if (major >= 12) {
            query = QString("SELECT * FROM %1 OFFSET %2 ROWS FETCH NEXT %3 ROWS ONLY")
                .arg(safeTable).arg(offset).arg(limit);
        } else {
            query = QString("SELECT * FROM (SELECT __p.*, ROWNUM __rn FROM (SELECT * FROM %1) __p "
                            "WHERE ROWNUM <= %2) WHERE __rn > %3")
                .arg(safeTable).arg(offset + limit).arg(offset);
        }
    } else {
        // SQL Server
        query = QString("SELECT * FROM %1 OFFSET %2 ROWS FETCH NEXT %3 ROWS ONLY")
            .arg(safeTable).arg(offset).arg(limit);
    }

    doQuery(query);
}

void DatabaseWorker::doVersion() {
    if (!QSqlDatabase::contains(m_connectionName)) return;
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) return;

    DatabaseClient::QueryResult result;
    result.success = true;
    result.columns << "version";

    QSqlQuery q(db);
    if (m_connectParams.dbType == "mysql" && q.exec("SELECT VERSION()") && q.next()) {
        result.rows.append(QVariantList{q.value(0)});
    } else if (m_connectParams.dbType == "postgres" && q.exec("SELECT version()") && q.next()) {
        result.rows.append(QVariantList{q.value(0)});
    } else if (m_connectParams.dbType == "sqlite" && q.exec("SELECT sqlite_version()") && q.next()) {
        result.rows.append(QVariantList{q.value(0)});
    }

    emit queryResult("SELECT VERSION()", result);
}

void DatabaseWorker::doExportData(const QString& table) {
    if (!QSqlDatabase::contains(m_connectionName)) { emit exportFinished(table, "未连接数据库"); return; }
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) { emit exportFinished(table, "连接已断开"); return; }

    QString safeTable = table;
    safeTable.remove(QRegularExpression("[^a-zA-Z0-9_\\.]"));
    QString sql;
    if (m_connectParams.dbType == "mysql")
        sql = QString("SELECT * FROM `%1`").arg(safeTable);
    else if (m_connectParams.dbType == "postgres" || m_connectParams.dbType == "sqlite")
        sql = QString("SELECT * FROM \"%1\"").arg(safeTable);
    else
        sql = QString("SELECT * FROM %1").arg(safeTable);

    QSqlQuery q(db);
    q.setForwardOnly(true);
    if (!q.exec(sql)) {
        emit exportFinished(table, q.lastError().text());
        return;
    }
    QSqlRecord rec = q.record();
    QStringList cols;
    for (int i = 0; i < rec.count(); ++i) cols << rec.fieldName(i);
    emit exportColumnsReady(table, cols);

    m_cancelRequested = false;
    qint64 fetched = 0;
    QList<QVariantList> chunk;
    chunk.reserve(2000);
    while (q.next()) {
        if (m_cancelRequested) {
            m_cancelRequested = false;
            emit exportFinished(table, QStringLiteral("__CANCELLED__"));
            return;
        }
        QVariantList row;
        row.reserve(rec.count());
        for (int i = 0; i < rec.count(); ++i) row.append(q.value(i));
        chunk.append(row);
        ++fetched;
        if (chunk.size() >= 2000) {
            emit exportChunk(table, chunk, fetched);
            chunk.clear();
            chunk.reserve(2000);
        }
    }
    if (!chunk.isEmpty()) emit exportChunk(table, chunk, fetched);
    emit exportFinished(table, QString());
}

void DatabaseWorker::doScriptBatch(const QString& sql, int batchIndex, int totalBatches) {
    if (batchIndex == 0) m_cancelRequested = false;  // 新导入会话：清除历史取消标志
    if (!QSqlDatabase::contains(m_connectionName)) {
        emit scriptBatchDone(batchIndex, totalBatches, false, "未连接数据库");
        return;
    }
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) {
        emit scriptBatchDone(batchIndex, totalBatches, false, "连接已断开");
        return;
    }

    QSqlQuery q(db);
    db.transaction();
    // 引号/注释感知的语句切分（字符串字面量内的 ; 不会误切）
    QStringList stmts;
    {
        QString cur;
        bool inS = false, inD = false, inLine = false, inBlock = false;
        const int n = sql.size();
        for (int i = 0; i < n; ++i) {
            const QChar c = sql[i];
            const QChar nx = (i + 1 < n) ? sql[i + 1] : QChar();
            if (inLine) { if (c == '\n') inLine = false; cur += c; continue; }
            if (inBlock) {
                if (c == '*' && nx == '/') { inBlock = false; cur += "*/"; ++i; }
                else cur += c;
                continue;
            }
            if (!inS && !inD) {
                if (c == '-' && nx == '-') { inLine = true; cur += c; continue; }
                if (c == '/' && nx == '*') { inBlock = true; cur += "/*"; ++i; continue; }
                if (c == '\'') inS = true;
                else if (c == '"') inD = true;
            } else if (inS && c == '\'') {
                if (nx == '\'') { cur += "''"; ++i; continue; }
                inS = false;
            } else if (inD && c == '"') {
                if (nx == '"') { cur += "\"\""; ++i; continue; }
                inD = false;
            }
            if (c == ';' && !inS && !inD) {
                const QString stmt = cur.trimmed();
                if (!stmt.isEmpty()) stmts << stmt;
                cur.clear();
                continue;
            }
            cur += c;
        }
        const QString tail = cur.trimmed();
        if (!tail.isEmpty()) stmts << tail;
    }
    for (const auto& s : stmts) {
        if (m_cancelRequested) {
            // 不重置标志：让队列里后续批次也全部跳过，实现整次导入取消
            db.rollback();
            emit scriptBatchDone(batchIndex, totalBatches, false, QStringLiteral("__CANCELLED__"));
            return;
        }
        if (!q.exec(s)) {
            const QString err = q.lastError().text();
            db.rollback();
            emit scriptBatchDone(batchIndex, totalBatches, false,
                                  QString("%1\n[语句] %2").arg(err, s.left(200)));
            return;
        }
    }
    if (!db.commit()) {
        emit scriptBatchDone(batchIndex, totalBatches, false, db.lastError().text());
        return;
    }
    emit scriptBatchDone(batchIndex, totalBatches, true, QString());
}

void DatabaseWorker::doFetchObjectSql(const QString& objectType, const QString& object) {
    auto fail = [&](const QString& msg) {
        emit objectSqlFetched(objectType, object, QString(), msg);
    };
    if (!QSqlDatabase::contains(m_connectionName)) { fail("未连接数据库"); return; }
    auto db = QSqlDatabase::database(m_connectionName);
    if (!db.isOpen()) { fail("连接已断开"); return; }

    const QString dbType = m_connectParams.dbType;

    // 拆分 schema.object
    QString schemaName, objName = object;
    const int dot = object.indexOf('.');
    if (dot > 0) {
        schemaName = object.left(dot);
        objName = object.mid(dot + 1);
    } else if (dbType == "postgres") schemaName = "public";
    else if (dbType == "oracle") schemaName = m_connectParams.username.toUpper();
    else if (dbType == "mssql") schemaName = "dbo";
    else schemaName = m_currentDatabase;

    auto esc = [](QString s) { s.replace('\'', QStringLiteral("''")); return s; };
    const QString schemaEsc = esc(schemaName);
    const QString objEsc = esc(objName);
    const QString fullEsc = esc(object);

    QString sql;
    QSqlQuery q(db);
    // 执行单值查询：失败或无行返回空串（用于多级回退）
    auto execOne = [&q](const QString& s) -> QString {
        if (!q.exec(s) || !q.next()) return QString();
        return q.value(0).toString().trimmed();
    };

    if (dbType == "postgres") {
        if (objectType == "view" || objectType == "matview") {
            const QString def = execOne(QString("SELECT pg_get_viewdef('%1'::regclass, true)").arg(fullEsc));
            if (!def.isEmpty()) {
                sql = (objectType == "matview")
                          ? QString("CREATE MATERIALIZED VIEW %1 AS\n%2;").arg(object, def)
                          : QString("CREATE OR REPLACE VIEW %1 AS\n%2;").arg(object, def);
            }
        } else if (objectType == "function" || objectType == "procedure") {
            const QString def = execOne(QString(
                "SELECT pg_get_functiondef(p.oid) FROM pg_proc p "
                "JOIN pg_namespace n ON n.oid = p.pronamespace "
                "WHERE p.proname = '%1' AND n.nspname = '%2'").arg(objEsc, schemaEsc));
            if (!def.isEmpty()) sql = def + ";";
        } else if (objectType == "trigger") {
            const QString def = execOne(QString(
                "SELECT pg_get_triggerdef(t.oid) FROM pg_trigger t "
                "JOIN pg_class c ON c.oid = t.tgrelid "
                "JOIN pg_namespace n ON n.oid = c.relnamespace "
                "WHERE t.tgname = '%1' AND n.nspname = '%2' AND NOT t.tgisinternal")
                .arg(objEsc, schemaEsc));
            if (!def.isEmpty()) sql = def + ";";
        } else if (objectType == "index") {
            const QString def = execOne(QString(
                "SELECT indexdef FROM pg_indexes WHERE indexname = '%1' AND schemaname = '%2'")
                .arg(objEsc, schemaEsc));
            if (!def.isEmpty()) sql = def + ";";
        } else if (objectType == "sequence") {
            sql = execOne(QString(
                "SELECT 'CREATE SEQUENCE ' || schemaname || '.' || sequencename || "
                "' START WITH ' || start_value || ' INCREMENT BY ' || increment_by || "
                "' MINVALUE ' || min_value || ' MAXVALUE ' || max_value || "
                "CASE WHEN cycle THEN ' CYCLE' ELSE ' NOCYCLE' END || ';' "
                "FROM pg_sequences WHERE schemaname = '%1' AND sequencename = '%2'")
                .arg(schemaEsc, objEsc));
        } else if (objectType == "type") {
            sql = execOne(QString(
                "SELECT 'CREATE TYPE ' || n.nspname || '.' || t.typname || ' AS ENUM (' || "
                "(SELECT string_agg(quote_literal(e.enumlabel), ', ' ORDER BY e.enumsortorder) "
                "FROM pg_enum e WHERE e.enumtypid = t.oid) || ');' "
                "FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace "
                "WHERE t.typname = '%1' AND n.nspname = '%2' AND t.typtype = 'e'")
                .arg(objEsc, schemaEsc));
        }
    } else if (dbType == "mysql") {
        QString show;
        int col = 2;  // SHOW CREATE FUNCTION/PROCEDURE/TRIGGER 的定义列
        if (objectType == "view") { show = QString("SHOW CREATE VIEW `%1`").arg(objEsc); col = 1; }
        else if (objectType == "function")   show = QString("SHOW CREATE FUNCTION `%1`").arg(objEsc);
        else if (objectType == "procedure")  show = QString("SHOW CREATE PROCEDURE `%1`").arg(objEsc);
        else if (objectType == "trigger")    show = QString("SHOW CREATE TRIGGER `%1`").arg(objEsc);
        if (!show.isEmpty()) {
            if (q.exec(show) && q.next()) {
                sql = q.value(col).toString().trimmed();
                if (!sql.isEmpty() && !sql.endsWith(';')) sql += ";";
            }
        }
    } else if (dbType == "sqlite") {
        if (objectType == "view" || objectType == "trigger" || objectType == "index") {
            const QString def = execOne(QString(
                "SELECT sql FROM sqlite_master WHERE name = '%1'").arg(objEsc));
            if (!def.isEmpty()) sql = def.endsWith(';') ? def : def + ";";
        }
    } else if (dbType == "oracle") {
        // 1) DBMS_METADATA（最完整，含存储参数）
        static const QHash<QString, QString> kMetaType = {
            {"view", "VIEW"}, {"matview", "MATERIALIZED_VIEW"}, {"sequence", "SEQUENCE"},
            {"procedure", "PROCEDURE"}, {"function", "FUNCTION"}, {"trigger", "TRIGGER"},
            {"index", "INDEX"}, {"synonym", "SYNONYM"}, {"type", "TYPE"},
            {"package", "PACKAGE"}, {"packagebody", "PACKAGE_BODY"}, {"dblink", "DB_LINK"},
        };
        const QString metaType = kMetaType.value(objectType);
        if (!metaType.isEmpty()) {
            // dlink 名称可能带域名后缀（如 MYLINK.WORLD），先全名后短名
            QStringList candidates{objName};
            if (objectType == "dblink" && objName.contains('.'))
                candidates << objName.left(objName.indexOf('.'));
            for (const auto& cand : candidates) {
                const QString def = execOne(QString(
                    "SELECT DBMS_METADATA.GET_DDL('%1', '%2', '%3') FROM DUAL")
                    .arg(metaType, esc(cand), schemaEsc));
                if (!def.isEmpty()) { sql = def; break; }
            }
        }
        // 2) 回退：ALL_SOURCE（存储过程/函数/包/触发器/类型源码）
        if (sql.isEmpty()) {
            static const QHash<QString, QString> kSrcType = {
                {"procedure", "PROCEDURE"}, {"function", "FUNCTION"}, {"package", "PACKAGE"},
                {"packagebody", "PACKAGE BODY"}, {"trigger", "TRIGGER"}, {"type", "TYPE"},
            };
            const QString srcType = kSrcType.value(objectType);
            if (!srcType.isEmpty() &&
                q.exec(QString("SELECT TEXT FROM ALL_SOURCE WHERE OWNER = '%1' AND NAME = '%2' "
                               "AND TYPE = '%3' ORDER BY LINE")
                           .arg(schemaEsc, objEsc, srcType))) {
                QStringList lines;
                while (q.next()) lines << q.value(0).toString();
                // ALL_SOURCE 每行不含换行符，需以 \n 拼接
                if (!lines.isEmpty()) sql = lines.join(QStringLiteral("\n")).trimmed();
            }
        }
        // 3) 回退：视图 / 物化视图
        if (sql.isEmpty() && objectType == "view") {
            QString def = execOne(QString("SELECT TEXT_VC FROM ALL_VIEWS "
                                          "WHERE OWNER = '%1' AND VIEW_NAME = '%2'")
                                      .arg(schemaEsc, objEsc));
            if (def.isEmpty())
                def = execOne(QString("SELECT TEXT FROM ALL_VIEWS "
                                      "WHERE OWNER = '%1' AND VIEW_NAME = '%2'")
                                  .arg(schemaEsc, objEsc));
            if (!def.isEmpty()) sql = QString("CREATE OR REPLACE VIEW %1 AS\n%2;").arg(object, def);
        }
        if (sql.isEmpty() && objectType == "matview") {
            const QString def = execOne(QString("SELECT QUERY FROM ALL_MVIEWS "
                                                "WHERE OWNER = '%1' AND MVIEW_NAME = '%2'")
                                            .arg(schemaEsc, objEsc));
            if (!def.isEmpty()) sql = QString("CREATE MATERIALIZED VIEW %1 AS\n%2;").arg(object, def);
        }
        // 4) 回退：序列
        if (sql.isEmpty() && objectType == "sequence") {
            sql = execOne(QString(
                "SELECT 'CREATE SEQUENCE ' || sequence_owner || '.' || sequence_name || "
                "' START WITH ' || last_number || ' INCREMENT BY ' || increment_by || "
                "CASE WHEN min_value = 1 THEN '' ELSE ' MINVALUE ' || min_value END || "
                "CASE WHEN cycle_flag = 'Y' THEN ' CYCLE' END || ';' "
                "FROM ALL_SEQUENCES WHERE sequence_owner = '%1' AND sequence_name = '%2'")
                .arg(schemaEsc, objEsc));
        }
        // 5) 回退：同义词
        if (sql.isEmpty() && objectType == "synonym") {
            sql = execOne(QString(
                "SELECT 'CREATE ' || DECODE(owner, 'PUBLIC', 'PUBLIC ') || 'SYNONYM ' || "
                "synonym_name || ' FOR ' || table_owner || '.' || table_name || ';' "
                "FROM ALL_SYNONYMS WHERE owner = '%1' AND synonym_name = '%2'")
                .arg(schemaEsc, objEsc));
        }
        // 6) 回退：DB 链接
        if (sql.isEmpty() && objectType == "dblink") {
            const QString shortName = objName.contains('.')
                                          ? objName.left(objName.indexOf('.')) : objName;
            sql = execOne(QString(
                "SELECT 'CREATE ' || DECODE(owner, 'PUBLIC', 'PUBLIC ') || 'DATABASE LINK ' || "
                "db_link || ' CONNECT TO ' || username || ' IDENTIFIED BY \"***\"' || "
                "CASE WHEN host IS NOT NULL THEN ' USING ''' || host || '''' END || ';' "
                "FROM ALL_DB_LINKS WHERE owner = '%1' AND (db_link = '%2' OR db_link LIKE '%3.%')")
                .arg(schemaEsc, esc(objName), esc(shortName)));
        }
    } else if (dbType == "mssql") {
        if (objectType == "view" || objectType == "procedure" ||
            objectType == "function" || objectType == "trigger") {
            sql = execOne(QString("SELECT definition FROM sys.sql_modules "
                                  "WHERE object_id = OBJECT_ID(N'%1')").arg(fullEsc));
        } else if (objectType == "sequence") {
            sql = execOne(QString(
                "SELECT 'CREATE SEQUENCE ' + QUOTENAME(s.name) + '.' + QUOTENAME(q.name) + "
                "' START WITH ' + CAST(q.start_value AS NVARCHAR(30)) + "
                "' INCREMENT BY ' + CAST(q.increment AS NVARCHAR(30)) + "
                "CASE WHEN q.is_cycling = 1 THEN ' CYCLE' ELSE ' NO CYCLE' END + ';' "
                "FROM sys.sequences q JOIN sys.schemas s ON s.schema_id = q.schema_id "
                "WHERE q.name = N'%1' AND s.name = N'%2'").arg(objEsc, schemaEsc));
        } else if (objectType == "index") {
            sql = execOne(QString(
                "SELECT 'CREATE ' + CASE WHEN i.is_unique = 1 THEN 'UNIQUE ' ELSE '' END + "
                "CASE i.type WHEN 1 THEN 'CLUSTERED ' WHEN 2 THEN 'NONCLUSTERED ' ELSE '' END + "
                "'INDEX ' + QUOTENAME(i.name) + ' ON ' + QUOTENAME(s.name) + '.' + "
                "QUOTENAME(t.name) + ' (' + STUFF((SELECT ',' + QUOTENAME(c.name) "
                "FROM sys.index_columns ic JOIN sys.columns c "
                "ON c.object_id = ic.object_id AND c.column_id = ic.column_id "
                "WHERE ic.object_id = i.object_id AND ic.index_id = i.index_id "
                "AND ic.is_included_column = 0 ORDER BY ic.key_ordinal FOR XML PATH('')), "
                "1, 1, '') + ');' "
                "FROM sys.indexes i JOIN sys.tables t ON t.object_id = i.object_id "
                "JOIN sys.schemas s ON s.schema_id = t.schema_id "
                "WHERE i.name = N'%1' AND s.name = N'%2' "
                "AND i.is_primary_key = 0 AND i.is_unique_constraint = 0")
                .arg(objEsc, schemaEsc));
        }
    }

    if (sql.isEmpty()) {
        fail(QString("无法获取对象 %1 的定义 SQL（可能权限不足、对象不存在，"
                     "或当前数据库类型不支持该类对象的 DDL 提取）").arg(object));
        return;
    }
    emit objectSqlFetched(objectType, object, sql, QString());
}
