#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QQueue>
#include <QVariantList>

struct redisContext;
struct redisReply;

/**
 * RedisClient: Redis 客户端（hiredis 封装）
 * 对应原 electron/redis.ts
 * 工作线程执行阻塞 hiredis 调用
 */
class RedisClient : public QObject {
    Q_OBJECT

public:
    // Redis 值类型
    enum RedisType { None, String, List, Hash, Set, ZSet, Stream };

    // 键信息
    struct KeyInfo {
        QString key;
        RedisType type = None;
        qint64 ttl = -1;          // -1 = 永久, -2 = 不存在
        qint64 size = 0;          // 集合元素数或字符串长度
    };

    // 命令结果
    struct CommandResult {
        bool success = false;
        QString error;
        QVariant value;           // 递归结构：QString/QVariantList/QVariantHash
        QString typeString;       // "string"/"array"/"integer"/"nil"/"status"
    };

    struct ConnectParams {
        QString host;
        int port = 6379;
        QString password;
        int database = 0;
        int timeoutSec = 10;
    };

    explicit RedisClient(QObject* parent = nullptr);
    ~RedisClient();

    // 公共 API
    void connectTo(const ConnectParams& params);
    void disconnect();
    void executeCommand(const QString& command);
    void scanKeys(const QString& pattern, int count = 200);
    void getKeyInfo(const QString& key);
    void deleteKey(const QString& key);
    void selectDatabase(int db);
    void getServerInfo();

    bool isConnected() const { return m_connected; }

signals:
    void connected(const QVariantMap& serverInfo);
    void disconnected();
    void connectionError(const QString& message);
    // 命令执行结果
    void commandResult(const QString& command, const CommandResult& result);
    // 键扫描结果
    void keysListed(const QList<KeyInfo>& keys, const QString& cursor);
    // 键详情
    void keyInfoReady(const KeyInfo& info, const QVariant& value);
    void keyDeleted(const QString& key, bool success);
    void databaseChanged(int db);
    void statusMessage(const QString& message);

private:
    void setConnected(bool v) { m_connected = v; }

    class RedisWorker* m_worker = nullptr;
    bool m_connected = false;
};

Q_DECLARE_METATYPE(RedisClient::CommandResult)
Q_DECLARE_METATYPE(RedisClient::KeyInfo)
Q_DECLARE_METATYPE(QList<RedisClient::KeyInfo>)

/**
 * RedisWorker: 工作线程
 */
class RedisWorker : public QThread {
    Q_OBJECT

public:
    explicit RedisWorker(QObject* parent = nullptr);
    ~RedisWorker();

    void setConnectParams(const RedisClient::ConnectParams& params) { m_connectParams = params; }
    void queueCommand(int type, const QString& a = QString(), const QString& b = QString(), int n = 0);
    void requestStop();

    // 命令类型
    enum CmdType {
        CmdConnect, CmdDisconnect, CmdExecute, CmdScan,
        CmdKeyInfo, CmdDelete, CmdSelect, CmdInfo
    };

signals:
    void connected(const QVariantMap& serverInfo);
    void disconnected();
    void connectionError(const QString& message);
    void commandResult(const QString& command, const RedisClient::CommandResult& result);
    void keysListed(const QList<RedisClient::KeyInfo>& keys, const QString& cursor);
    void keyInfoReady(const RedisClient::KeyInfo& info, const QVariant& value);
    void keyDeleted(const QString& key, bool success);
    void databaseChanged(int db);
    void statusMessage(const QString& message);

protected:
    void run() override;

private:
    struct Command {
        int type;
        QString a, b;
        int n = 0;
    };

    bool doConnect(const RedisClient::ConnectParams& params);
    void doDisconnect();
    void doExecute(const QString& command);
    void doScan(const QString& pattern, int count);
    void doKeyInfo(const QString& key);
    void doDelete(const QString& key);
    void doSelect(int db);
    void doInfo();

    // hiredis 辅助
    QVariant replyToVariant(redisReply* reply);
    RedisClient::RedisType typeFromString(const QString& typeStr);

    redisContext* m_ctx = nullptr;
    RedisClient::ConnectParams m_connectParams;
    bool m_running = false;

    QMutex m_mutex;
    QQueue<Command> m_queue;
};
