#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

/**
 * CacheDb: 程序本地缓存库（独立 SQLite 文件）
 *
 *   - 路径：%APPDATA%/bambooRat/cache.sqlite
 *   - 多账号共用同一文件；按 acc_id 区分
 *   - 仅本进程写；写前自动外加 200ms 互斥锁（避免多线程同时写）
 *
 * 数据导入完全由 WeChatSyncWorker 负责；本类只负责：
 *   1) 建表（首次启动）
 *   2) 写入（replace 全表 + 单行 upsert）
 *   3) 查询（O(1) 级本地 SQLite，前端直接调用）
 *   4) 同步状态（增量识别）
 */
class CacheDb {
public:
    // 数据库路径
    static QString dbPath();

    // 初始化（首次启动建表）。失败时 *errOut 接收错误。
    static bool initialize(QString* errOut = nullptr);

    // ── 账号 ────────────────────────────────────────────────────────
    static bool upsertAccount(const QString& accId, const QString& name,
                              const QString& wxid, const QString& dataDir,
                              const QString& keyHex);
    static QList<QVariantMap> loadAccounts();
    static bool deleteAccount(const QString& accId);

    // ── 会话 ────────────────────────────────────────────────────────
    // 清空某账号所有会话后写入（事务）
    static bool replaceSessions(const QString& accId, const QVariantList& rows);
    // 增量更新（按 acc_id+talker upsert）
    static bool upsertSessions(const QString& accId, const QVariantList& rows);
    static QVariantList loadSessions(const QString& accId, int limit = 0);

    // ── 联系人 ──────────────────────────────────────────────────────
    static bool replaceContacts(const QString& accId, const QVariantList& rows);
    static bool upsertContacts(const QString& accId, const QVariantList& rows);
    static QVariantList loadContacts(const QString& accId);

    // ── 消息 ────────────────────────────────────────────────────────
    static bool replaceMessages(const QString& accId, const QString& talker,
                                const QList<QVariantMap>& rows);
    static bool upsertMessages(const QString& accId, const QString& talker,
                               const QList<QVariantMap>& rows);
    static QList<QVariantMap> loadMessages(const QString& accId, const QString& talker,
                                            int limit = 0);

    // 从 XML content 中提取附件元信息（type=49 XML 复合消息等）
    // 输出到 m["attachTitle"] / m["attachSize"] / m["attachExt"] / m["attachUrl"] / m["attachMime"]
    static void parseAttachMeta(int type, int subType,
                               const QString& content, QVariantMap& m);

    // ── 群成员 ──────────────────────────────────────────────────────
    static bool replaceChatRoomMembers(const QString& accId, const QString& chatRoomId,
                                       const QStringList& wxids);
    static QStringList loadChatRoomMembers(const QString& accId, const QString& chatRoomId);

    // ── 同步状态（用于增量识别） ─────────────────────────────────────
    static bool getSyncState(const QString& accId, const QString& sourcePath,
                             qint64* sizeOut, qint64* mtimeOut);
    static bool setSyncState(const QString& accId, const QString& sourcePath,
                             qint64 size, qint64 mtime);
    static bool clearSyncState(const QString& accId);

    // ── 内容指纹（信号层去重：worker 写完后对比 hash，未变则不 emit UI） ──
    // kind: "sessions" / "contacts"
    static QString getContentHash(const QString& accId, const QString& kind);
    static bool    setContentHash(const QString& accId, const QString& kind,
                                  const QString& hash);
};