#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

/**
 * WeChatSyncWorker: 后台同步 worker（独立线程）
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │  UI 线程                                              │
 *   │   · 点击文件夹 → CacheDb 读（O(1)）                  │
 *   │   · 启动时 syncAll() 排队                             │
 *   └──────────────────────────────────────────────────────┘
 *                              ↕ 跨线程信号
 *   ┌──────────────────────────────────────────────────────┐
 *   │  SyncWorker 线程（独立 QThread）                      │
 *   │   1. 启动时遍历账号 → 增量同步                         │
 *   │   2. QFileSystemWatcher 监控 dataDir                   │
 *   │      文件变化 → 重新解密 → upsert 到 CacheDb           │
 *   │   3. 流式 emit syncProgress → UI 实时刷新              │
 *   └──────────────────────────────────────────────────────┘
 *
 * 信号（线程间通信，Qt::QueuedConnection 自动回到主线程）：
 *   syncStarted(accId, stage)              阶段开始
 *   syncProgress(accId, stage, cur, total) 阶段进度（流式输出）
 *   syncAccountDataReady(accId, sessions, contacts) 同步后数据（替换 UI）
 *   syncFinished(accId, elapsedMs)
 *   syncFailed(accId, reason)
 *
 * 槽（主线程发起，QueuedConnection 到 worker 线程）：
 *   syncAccount(accId)
 *   syncAll()
 *   watchAccount(accId) / unwatchAccount(accId)
 */
class WeChatSyncWorker : public QObject {
    Q_OBJECT

public:
    explicit WeChatSyncWorker(QObject* parent = nullptr);

public slots:
    // 同步单个账号：解密 → 增量 upsert 到 CacheDb → 回调
    void syncAccount(const QString& accId);

    // 同步所有已知账号
    void syncAll();

    // 启动 watcher（文件变化时自动 sync）
    void watchAccount(const QString& accId);

    // 停止 watcher
    void unwatchAccount(const QString& accId);

    // 单联系人详细信息异步加载（在 worker 线程，不阻塞 UI）
    // 完成时 emit contactDetailReady
    void loadContactDetail(const QString& accId, const QString& wxid);

signals:
    void syncStarted(const QString& accId, const QString& stage);
    void syncProgress(const QString& accId, const QString& stage,
                      int current, int total, const QString& msg);
    void syncAccountDataReady(const QString& accId,
                              const QVariantList& sessions,
                              const QVariantList& contacts);
    void syncMessagesReady(const QString& accId, const QString& talker,
                           const QString& title,
                           const QList<QVariantMap>& messages);
    void syncFinished(const QString& accId, qint64 elapsedMs);
    void syncFailed(const QString& accId, const QString& reason);

    // 联系人详细信息异步返回
    void contactDetailReady(const QString& accId, const QString& wxid,
                            const QVariantMap& detail);

    // watcher 回调（worker 线程触发，已在 worker 线程）
    void dirChanged(const QString& accId);

private slots:
    void onWatcherChanged(const QString& path);

private:
    // 同步账号元数据（联系人 + 会话）
    bool syncAccountMeta(const QString& accId);

    // 同步某会话的消息（解密后 upsert 到 CacheDb）
    bool syncAccountMessages(const QString& accId);

    // 列出某账号的所有群聊 id（用于同步群成员）
    QStringList listChatRoomIds(const QString& accId);

    // 检查源文件 mtime 是否变化（用于跳过未变化的 db）
    bool needsResync(const QString& accId, const QString& sourcePath,
                     qint64 size, qint64 mtime);

    QFileSystemWatcher* m_watcher = nullptr;
    QHash<QString, QStringList> m_watchedDirs;  // accId -> watched paths
    QSet<QString> m_syncing;                   // 防重入：当前正在同步的账号
};