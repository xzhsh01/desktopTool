#include "redis/RedisClient.h"
#include "core/Logger.h"

#include <QVariantMap>
#include <QVariantHash>
#include <QVariantList>

#ifdef HAVE_HIREDIS
#include <hiredis/hiredis.h>
#endif

// 注册元类型（用于队列信号传参）
static const bool regTypes = []() {
    qRegisterMetaType<RedisClient::CommandResult>("RedisClient::CommandResult");
    qRegisterMetaType<RedisClient::KeyInfo>("RedisClient::KeyInfo");
    qRegisterMetaType<QList<RedisClient::KeyInfo>>("QList<RedisClient::KeyInfo>");
    return true;
}();

// ── RedisClient（主线程侧） ───────────────────────────────────────────────────

RedisClient::RedisClient(QObject* parent) : QObject(parent) {}

RedisClient::~RedisClient() {
    disconnect();
}

void RedisClient::connectTo(const ConnectParams& params) {
    if (m_worker && m_worker->isRunning()) {
        emit connectionError("已有 Redis 连接，请先断开");
        return;
    }
    if (m_worker) {
        m_worker->deleteLater();
        m_worker = nullptr;
    }

    m_worker = new RedisWorker(this);
    connect(m_worker, &RedisWorker::connected, this, [this](const QVariantMap& info) {
        setConnected(true);
        emit connected(info);
    });
    connect(m_worker, &RedisWorker::disconnected, this, [this]() {
        setConnected(false);
        emit disconnected();
    });
    connect(m_worker, &RedisWorker::connectionError, this, &RedisClient::connectionError);
    connect(m_worker, &RedisWorker::commandResult, this, &RedisClient::commandResult);
    connect(m_worker, &RedisWorker::keysListed, this, &RedisClient::keysListed);
    connect(m_worker, &RedisWorker::keyInfoReady, this, &RedisClient::keyInfoReady);
    connect(m_worker, &RedisWorker::keyDeleted, this, &RedisClient::keyDeleted);
    connect(m_worker, &RedisWorker::databaseChanged, this, &RedisClient::databaseChanged);
    connect(m_worker, &RedisWorker::statusMessage, this, &RedisClient::statusMessage);
    connect(m_worker, &RedisWorker::finished, this, [this]() {
        setConnected(false);
        m_worker->deleteLater();
        m_worker = nullptr;
    });

    // Connect 命令通过 queueCommand 无法携带完整参数，这里扩展：
    // 直接用自定义结构传递
    // 简化：把参数编码进命令队列（a = host, b = password, n = port*1000+db 不够精确）
    // 改为在 worker 中设置参数
    m_worker->setConnectParams(params);
    m_worker->queueCommand(RedisWorker::CmdConnect);
    m_worker->start();
}

void RedisClient::disconnect() {
    if (m_worker) {
        m_worker->queueCommand(RedisWorker::CmdDisconnect);
        m_worker->requestStop();
    }
}

void RedisClient::executeCommand(const QString& command) {
    if (m_worker) m_worker->queueCommand(RedisWorker::CmdExecute, command);
}

void RedisClient::scanKeys(const QString& pattern, int count) {
    if (m_worker) m_worker->queueCommand(RedisWorker::CmdScan, pattern, QString(), count);
}

void RedisClient::getKeyInfo(const QString& key) {
    if (m_worker) m_worker->queueCommand(RedisWorker::CmdKeyInfo, key);
}

void RedisClient::deleteKey(const QString& key) {
    if (m_worker) m_worker->queueCommand(RedisWorker::CmdDelete, key);
}

void RedisClient::selectDatabase(int db) {
    if (m_worker) m_worker->queueCommand(RedisWorker::CmdSelect, QString(), QString(), db);
}

void RedisClient::getServerInfo() {
    if (m_worker) m_worker->queueCommand(RedisWorker::CmdInfo);
}

// ── RedisWorker（工作线程侧） ─────────────────────────────────────────────────

RedisWorker::RedisWorker(QObject* parent) : QThread(parent) {}

RedisWorker::~RedisWorker() {
    requestStop();
    wait(3000);
}

void RedisWorker::queueCommand(int type, const QString& a, const QString& b, int n) {
    QMutexLocker locker(&m_mutex);
    Command cmd{type, a, b, n};
    m_queue.enqueue(cmd);
}

void RedisWorker::requestStop() {
    m_running = false;
}

void RedisWorker::run() {
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
                case CmdExecute:  doExecute(cmd.a); break;
                case CmdScan:      doScan(cmd.a, cmd.n); break;
                case CmdKeyInfo:   doKeyInfo(cmd.a); break;
                case CmdDelete:    doDelete(cmd.a); break;
                case CmdSelect:    doSelect(cmd.n); break;
                case CmdInfo:      doInfo(); break;
            }
        }

        msleep(m_ctx ? 50 : 100);
    }

    doDisconnect();
    emit disconnected();
}

bool RedisWorker::doConnect(const RedisClient::ConnectParams& params) {
#ifdef HAVE_HIREDIS
    struct timeval tv = {params.timeoutSec, 0};
    m_ctx = redisConnectWithTimeout(params.host.toUtf8().constData(), params.port, tv);
    if (!m_ctx || m_ctx->err) {
        QString err = m_ctx ? QString::fromUtf8(m_ctx->errstr) : "分配上下文失败";
        if (m_ctx) redisFree(m_ctx);
        m_ctx = nullptr;
        emit connectionError(QString("无法连接 Redis %1:%2 - %3")
            .arg(params.host).arg(params.port).arg(err));
        return false;
    }
    redisSetTimeout(m_ctx, tv);

    // 认证（有密码时）
    if (!params.password.isEmpty()) {
        redisReply* reply = static_cast<redisReply*>(
            redisCommand(m_ctx, "AUTH %s", params.password.toUtf8().constData()));
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            QString err = reply ? QString::fromUtf8(reply->str) : "无响应";
            if (reply) freeReplyObject(reply);
            redisFree(m_ctx);
            m_ctx = nullptr;
            emit connectionError(QString("Redis 认证失败: %1").arg(err));
            return false;
        }
        freeReplyObject(reply);
    }

    // 选择数据库
    if (params.database > 0) {
        redisReply* reply = static_cast<redisReply*>(
            redisCommand(m_ctx, "SELECT %d", params.database));
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            if (reply) freeReplyObject(reply);
            redisFree(m_ctx);
            m_ctx = nullptr;
            emit connectionError(QString("无法切换到数据库 %1").arg(params.database));
            return false;
        }
        freeReplyObject(reply);
    }

    // 获取服务器信息
    QVariantMap info;
    redisReply* infoReply = static_cast<redisReply*>(redisCommand(m_ctx, "INFO"));
    if (infoReply && infoReply->type == REDIS_REPLY_STRING) {
        // 解析 INFO 输出的部分字段
        QString infoStr = QString::fromUtf8(infoReply->str, static_cast<int>(infoReply->len));
        for (const QString& line : infoStr.split('\n')) {
            int idx = line.indexOf(':');
            if (idx > 0) {
                QString key = line.left(idx).trimmed();
                QString value = line.mid(idx + 1).trimmed();
                if (key == "redis_version" || key == "os" ||
                    key == "tcp_port" || key == "connected_clients" ||
                    key == "used_memory_human" || key == "uptime_in_days") {
                    info[key] = value;
                }
            }
        }
    }
    if (infoReply) freeReplyObject(infoReply);

    Logger::instance().info(QString("Redis 已连接: %1:%2 db=%3")
        .arg(params.host).arg(params.port).arg(params.database), "redis");
    emit statusMessage("已连接");
    emit connected(info);
    return true;

#else
    Q_UNUSED(params)
    emit connectionError("此构建未启用 hiredis 支持（HAVE_HIREDIS 未定义）");
    return false;
#endif
}

void RedisWorker::doDisconnect() {
#ifdef HAVE_HIREDIS
    if (m_ctx) {
        redisFree(m_ctx);
        m_ctx = nullptr;
    }
#endif
}

void RedisWorker::doExecute(const QString& command) {
#ifdef HAVE_HIREDIS
    if (!m_ctx) {
        RedisClient::CommandResult result;
        result.success = false;
        result.error = "未连接";
        emit commandResult(command, result);
        return;
    }

    redisReply* reply = static_cast<redisReply*>(
        redisCommand(m_ctx, command.toUtf8().constData()));

    RedisClient::CommandResult result;
    if (!reply) {
        result.success = false;
        result.error = QString("连接错误: %1").arg(m_ctx->errstr);
        // 连接失效
        doDisconnect();
        m_running = false;
    } else {
        result.success = (reply->type != REDIS_REPLY_ERROR);
        if (reply->type == REDIS_REPLY_ERROR) {
            result.error = QString::fromUtf8(reply->str, static_cast<int>(reply->len));
        }
        result.value = replyToVariant(reply);
        switch (reply->type) {
            case REDIS_REPLY_STRING: result.typeString = "string"; break;
            case REDIS_REPLY_ARRAY:  result.typeString = "array"; break;
            case REDIS_REPLY_INTEGER: result.typeString = "integer"; break;
            case REDIS_REPLY_NIL:    result.typeString = "nil"; break;
            case REDIS_REPLY_STATUS: result.typeString = "status"; break;
            case REDIS_REPLY_ERROR:  result.typeString = "error"; break;
        }
        freeReplyObject(reply);
    }

    emit commandResult(command, result);
#else
    Q_UNUSED(command)
#endif
}

void RedisWorker::doScan(const QString& pattern, int count) {
#ifdef HAVE_HIREDIS
    if (!m_ctx) return;

    // SCAN 0 MATCH pattern COUNT count
    // 使用游标遍历（这里简化为单次 SCAN，返回第一页）
    redisReply* reply = static_cast<redisReply*>(redisCommand(
        m_ctx, "SCAN 0 MATCH %s COUNT %d",
        pattern.toUtf8().constData(), count));

    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
        if (reply) freeReplyObject(reply);
        emit keysListed({}, "0");
        return;
    }

    // reply[0] = 新游标, reply[1] = 键列表
    QString cursor = QString::fromUtf8(
        reply->element[0]->str, static_cast<int>(reply->element[0]->len));

    QList<RedisClient::KeyInfo> keys;
    redisReply* keysReply = reply->element[1];
    for (size_t i = 0; i < keysReply->elements; ++i) {
        RedisClient::KeyInfo info;
        info.key = QString::fromUtf8(
            keysReply->element[i]->str, static_cast<int>(keysReply->element[i]->len));

        // 获取类型（TYPE key）
        redisReply* typeReply = static_cast<redisReply*>(
            redisCommand(m_ctx, "TYPE %s", info.key.toUtf8().constData()));
        if (typeReply && typeReply->type == REDIS_REPLY_STATUS) {
            info.type = typeFromString(QString::fromUtf8(typeReply->str, static_cast<int>(typeReply->len)));
        }
        if (typeReply) freeReplyObject(typeReply);

        // TTL
        redisReply* ttlReply = static_cast<redisReply*>(
            redisCommand(m_ctx, "TTL %s", info.key.toUtf8().constData()));
        if (ttlReply && ttlReply->type == REDIS_REPLY_INTEGER) {
            info.ttl = ttlReply->integer;
        }
        if (ttlReply) freeReplyObject(ttlReply);

        keys.append(info);
    }
    freeReplyObject(reply);

    // 按键名排序
    std::sort(keys.begin(), keys.end(),
              [](const RedisClient::KeyInfo& a, const RedisClient::KeyInfo& b) {
        return a.key.compare(b.key, Qt::CaseInsensitive) < 0;
    });

    emit keysListed(keys, cursor);

#else
    Q_UNUSED(pattern)
    Q_UNUSED(count)
#endif
}

void RedisWorker::doKeyInfo(const QString& key) {
#ifdef HAVE_HIREDIS
    if (!m_ctx) return;

    RedisClient::KeyInfo info;
    info.key = key;

    // 类型
    redisReply* typeReply = static_cast<redisReply*>(
        redisCommand(m_ctx, "TYPE %s", key.toUtf8().constData()));
    if (typeReply && typeReply->type == REDIS_REPLY_STATUS) {
        info.type = typeFromString(QString::fromUtf8(typeReply->str, static_cast<int>(typeReply->len)));
    }
    if (typeReply) freeReplyObject(typeReply);

    // TTL
    redisReply* ttlReply = static_cast<redisReply*>(
        redisCommand(m_ctx, "TTL %s", key.toUtf8().constData()));
    if (ttlReply && ttlReply->type == REDIS_REPLY_INTEGER) {
        info.ttl = ttlReply->integer;
    }
    if (ttlReply) freeReplyObject(ttlReply);

    // 根据类型获取值
    QVariant value;
    QByteArray keyUtf8 = key.toUtf8();
    const char* k = keyUtf8.constData();

    switch (info.type) {
        case RedisClient::String: {
            redisReply* r = static_cast<redisReply*>(redisCommand(m_ctx, "GET %s", k));
            if (r && r->type == REDIS_REPLY_STRING) {
                value = QString::fromUtf8(r->str, static_cast<int>(r->len));
                info.size = static_cast<qint64>(r->len);
            }
            if (r) freeReplyObject(r);
            break;
        }
        case RedisClient::List: {
            redisReply* r = static_cast<redisReply*>(redisCommand(m_ctx, "LRANGE %s 0 -1", k));
            if (r && r->type == REDIS_REPLY_ARRAY) {
                value = replyToVariant(r);
                info.size = static_cast<qint64>(r->elements);
            }
            if (r) freeReplyObject(r);
            break;
        }
        case RedisClient::Hash: {
            redisReply* r = static_cast<redisReply*>(redisCommand(m_ctx, "HGETALL %s", k));
            if (r && r->type == REDIS_REPLY_ARRAY) {
                QVariantHash hash;
                for (size_t i = 0; i + 1 < r->elements; i += 2) {
                    hash[QString::fromUtf8(r->element[i]->str, static_cast<int>(r->element[i]->len))] =
                        QString::fromUtf8(r->element[i+1]->str, static_cast<int>(r->element[i+1]->len));
                }
                value = hash;
                info.size = static_cast<qint64>(r->elements / 2);
            }
            if (r) freeReplyObject(r);
            break;
        }
        case RedisClient::Set: {
            redisReply* r = static_cast<redisReply*>(redisCommand(m_ctx, "SMEMBERS %s", k));
            if (r && r->type == REDIS_REPLY_ARRAY) {
                value = replyToVariant(r);
                info.size = static_cast<qint64>(r->elements);
            }
            if (r) freeReplyObject(r);
            break;
        }
        case RedisClient::ZSet: {
            redisReply* r = static_cast<redisReply*>(redisCommand(m_ctx, "ZRANGE %s 0 -1 WITHSCORES", k));
            if (r && r->type == REDIS_REPLY_ARRAY) {
                QVariantList list;
                for (size_t i = 0; i + 1 < r->elements; i += 2) {
                    QVariantHash pair;
                    pair["member"] = QString::fromUtf8(r->element[i]->str, static_cast<int>(r->element[i]->len));
                    pair["score"] = QString::fromUtf8(r->element[i+1]->str, static_cast<int>(r->element[i+1]->len));
                    list.append(pair);
                }
                value = list;
                info.size = static_cast<qint64>(r->elements / 2);
            }
            if (r) freeReplyObject(r);
            break;
        }
        default:
            break;
    }

    emit keyInfoReady(info, value);

#else
    Q_UNUSED(key)
#endif
}

void RedisWorker::doDelete(const QString& key) {
#ifdef HAVE_HIREDIS
    if (!m_ctx) {
        emit keyDeleted(key, false);
        return;
    }
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(m_ctx, "DEL %s", key.toUtf8().constData()));
    bool success = reply && reply->type == REDIS_REPLY_INTEGER && reply->integer > 0;
    if (reply) freeReplyObject(reply);
    if (success) {
        Logger::instance().info(QString("已删除键: %1").arg(key), "redis");
    }
    emit keyDeleted(key, success);
#else
    Q_UNUSED(key)
    emit keyDeleted(key, false);
#endif
}

void RedisWorker::doSelect(int db) {
#ifdef HAVE_HIREDIS
    if (!m_ctx) return;
    redisReply* reply = static_cast<redisReply*>(redisCommand(m_ctx, "SELECT %d", db));
    bool success = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply) freeReplyObject(reply);
    if (success) {
        emit databaseChanged(db);
    }
#else
    Q_UNUSED(db)
#endif
}

void RedisWorker::doInfo() {
#ifdef HAVE_HIREDIS
    if (!m_ctx) return;
    // INFO 命令已在连接时获取，这里重新获取
    redisReply* reply = static_cast<redisReply*>(redisCommand(m_ctx, "INFO"));
    if (reply && reply->type == REDIS_REPLY_STRING) {
        // 简单：通过 commandResult 发送
        RedisClient::CommandResult result;
        result.success = true;
        result.typeString = "string";
        result.value = QString::fromUtf8(reply->str, static_cast<int>(reply->len));
        emit commandResult("INFO", result);
    }
    if (reply) freeReplyObject(reply);
#endif
}

QVariant RedisWorker::replyToVariant(redisReply* reply) {
#ifdef HAVE_HIREDIS
    if (!reply) return {};

    switch (reply->type) {
        case REDIS_REPLY_STRING:
        case REDIS_REPLY_STATUS:
            return QString::fromUtf8(reply->str, static_cast<int>(reply->len));
        case REDIS_REPLY_INTEGER:
            return static_cast<qlonglong>(reply->integer);
        case REDIS_REPLY_NIL:
            return QVariant();
        case REDIS_REPLY_ARRAY: {
            QVariantList list;
            for (size_t i = 0; i < reply->elements; ++i) {
                list.append(replyToVariant(reply->element[i]));
            }
            return list;
        }
        case REDIS_REPLY_ERROR:
            return QString::fromUtf8(reply->str, static_cast<int>(reply->len));
        default:
            return {};
    }
#else
    Q_UNUSED(reply)
    return {};
#endif
}

RedisClient::RedisType RedisWorker::typeFromString(const QString& typeStr) {
    if (typeStr == "string") return RedisClient::String;
    if (typeStr == "list") return RedisClient::List;
    if (typeStr == "hash") return RedisClient::Hash;
    if (typeStr == "set") return RedisClient::Set;
    if (typeStr == "zset") return RedisClient::ZSet;
    if (typeStr == "stream") return RedisClient::Stream;
    return RedisClient::None;
}
