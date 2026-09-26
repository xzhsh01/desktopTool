#pragma once

#include <QObject>
#include <QTimer>
#include <QSemaphore>
#include <QThread>
#include <QHash>
#include <QString>
#include <QList>
#include <QSet>
#include <QDateTime>

#include "mail/MailAccountManager.h"
#include "mail/ImapClient.h"

class QSslSocket;

/**
 * MailPoller: 后台 IMAP 同步调度器
 *
 * 两条并行职责：
 *   1) INBOX 轻量轮询（默认 1 分钟）
 *      - 每个账号一个独立线程，只拉 INBOX 未读
 *      - 用于新邮件通知（emit notify） + UI 角标更新
 *      - 间隔 Settings("mail.pollIntervalMin")
 *
 *   2) 全量同步（默认 5 分钟）
 *      - 两阶段：
 *          阶段①：每个账号一个独立线程跑 IMAP LIST 文件夹
 *          阶段②：阶段①全部完成后，每个 (账号, 文件夹) 对一个独立线程跑 fetchHeaders
 *      - 全局并发上限 m_folderSem=3（防 139 等服务器限流）
 *      - 轮次号 m_fullSyncRound 用于丢弃过期回调（停止 / 重启时安全取消）
 *      - 间隔 Settings("mail.fullSyncIntervalMin")
 */
class MailPoller : public QObject {
    Q_OBJECT

public:
    static MailPoller& instance();

    // 启动 / 停止（同时启停 INBOX 轮询 + 全量同步定时器）
    void start();
    void stop();

    // 立即拉取所有账号 INBOX（手动触发，用于新邮件通知）
    void pollNow();

    // 立即拉取指定账号 INBOX（窗口激活 / 切换文件夹时）
    void pollAccount(const QString& accountId);

    // 立即触发一次全量同步（若已有全量同步在进行中则忽略）
    void runFullSyncNow();

    // 上次刷新时间（任一账号）—— 用于 UI 显示
    QDateTime lastRefreshTime() const { return m_lastRefreshTime; }

signals:
    // 通知 UI 显示气泡 / 播放声音（INBOX 新邮件）
    void notify(const QString& accountName, const QString& subject, const QString& from);

    // 任一账号 worker 完成（成功或失败都会发）。MailSelfTest 等所有 worker 完成。
    void accountPolled(const QString& accountId);

    // ── 全量同步阶段信号（外部可选监听）──
    void fullSyncStarted(int totalAccounts);                    // 阶段① 开始
    void fullSyncFoldersListed(int totalFolders);               // 阶段② 开始
    void fullSyncFinished(int succeededFolders, int totalFolders); // 全部完成

private slots:
    void onTimerTick();                     // INBOX 轮询定时器
    void onWorkerFinished(const QString& accountId);
    void onFullSyncTimerTick();             // 全量同步定时器

private:
    MailPoller(QObject* parent = nullptr);
    MailPoller(const MailPoller&) = delete;
    MailPoller& operator=(const MailPoller&) = delete;

    struct Worker {
        QThread*  thread = nullptr;
        QString   accountId;
    };

    // 启动单个账号的 INBOX worker
    bool startWorker(const MailAccountManager::Account& a);

    // ── 全量同步 ──
    void startFullSyncCycle(int intervalMin);   // 启定时器
    void stopFullSyncCycle();                   // 停定时器 + 作废当前轮次
    void beginFullSyncRound();                  // 开新一轮（阶段①）
    // 阶段① 回调（主线程）
    void onAccountListDone(int round, const QString& accountId, bool ok,
                           const QList<ImapClient::Folder>& folders, const QString& err);
    // 阶段② 回调（主线程）
    void onFolderSyncDone(int round, const QString& accountId,
                          const QString& folder, bool ok, const QString& err);

    // INBOX 轮询
    QTimer m_timer;
    QHash<QString, Worker> m_workers;
    QDateTime m_lastRefreshTime;

    // 全量同步状态
    QTimer   m_fullSyncTimer;
    QSemaphore m_folderSem{3};                 // 阶段② 全局并发上限（防服务器限流）
    int      m_fullSyncRound   = 0;            // 轮次号：丢弃过期回调
    bool     m_fullSyncActive  = false;        // 是否正在全量同步
    int      m_fullSyncPendingAccounts = 0;    // 阶段① 待完成账号数
    int      m_fullSyncFoldersTotal    = 0;    // 阶段② 总任务数
    int      m_fullSyncFoldersDone     = 0;    // 阶段② 已完成数
    int      m_fullSyncFoldersSucceeded = 0;   // 阶段② 成功数
    QHash<QString, QList<ImapClient::Folder>> m_fullSyncFolders;  // 阶段① 结果缓存
};

/**
 * MailWorker: 单个账号的 INBOX IMAP 拉取工作（运行在工作线程）
 */
class MailWorker : public QObject {
    Q_OBJECT
public:
    explicit MailWorker(QString accountId, QObject* parent = nullptr);

public slots:
    void run();

signals:
    void finished(const QString& accountId);
    void notify(const QString& accountName, const QString& subject, const QString& from);

private:
    QString m_accountId;
};