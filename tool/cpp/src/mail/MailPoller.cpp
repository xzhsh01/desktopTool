#include "mail/MailPoller.h"
#include "mail/MailAccountManager.h"
#include "mail/MailStore.h"
#include "mail/ImapClient.h"
#include "core/Settings.h"
#include "core/Logger.h"

#include <QUuid>
#include <QThread>
#include <QMetaObject>

// ── MailPoller ───────────────────────────────────────────────────────────────

MailPoller& MailPoller::instance() {
    static MailPoller p;
    return p;
}

MailPoller::MailPoller(QObject* parent) : QObject(parent) {
    connect(&m_timer, &QTimer::timeout, this, &MailPoller::onTimerTick);
    connect(&m_fullSyncTimer, &QTimer::timeout, this, &MailPoller::onFullSyncTimerTick);
}

void MailPoller::start() {
    // ① INBOX 轻量轮询：用于新邮件通知
    int mins = Settings::instance().get("mail.pollIntervalMin", 1).toInt();
    if (mins < 1) mins = 1;
    m_timer.start(mins * 60 * 1000);
    Logger::instance().info(
        QString("邮件 INBOX 轮询已启动，间隔 %1 分钟").arg(mins), "mail");
    // 启动时立即拉一次
    pollNow();

    // ② 全量同步定时器：阶段① LIST 文件夹 → 阶段② 每文件夹一线程 fetchHeaders
    int fsMins = Settings::instance().get("mail.fullSyncIntervalMin", 5).toInt();
    if (fsMins < 1) fsMins = 5;
    startFullSyncCycle(fsMins);
}

void MailPoller::stop() {
    Logger::instance().info(
        QString("邮件轮询已停止 (运行中 worker=%1)").arg(m_workers.size()), "mail");
    m_timer.stop();
    stopFullSyncCycle();
    // 不在调用线程（通常是 UI 线程）wait+delete：worker 的 IMAP 拉取可能持续数十秒，
    // 强等会卡死界面；且 wait 超时后 delete 仍在运行的 QThread 会直接崩溃。
    // 仅请求线程退出事件循环，worker 跑完后经 finished → deleteLater 自销毁。
    for (auto it = m_workers.begin(); it != m_workers.end(); ++it) {
        if (it->thread) it->thread->quit();
    }
    m_workers.clear();
}

void MailPoller::pollNow() {
    const auto& accounts = MailAccountManager::instance().accounts();
    Logger::instance().info(
        QString("pollNow() 触发: 共 %1 个账号").arg(accounts.size()), "mail");

    int started = 0, skipped = 0;
    for (const auto& a : accounts) {
        if (startWorker(a)) ++started;
        else                ++skipped;
    }
    Logger::instance().info(
        QString("pollNow() 调度完成: 启动=%1 跳过=%2").arg(started).arg(skipped),
        "mail");
}

void MailPoller::pollAccount(const QString& accountId) {
    if (accountId.isEmpty()) return;
    auto* acc = MailAccountManager::instance().getById(accountId);
    if (!acc) {
        Logger::instance().warn(
            QString("pollAccount(): 账号不存在 id=%1").arg(accountId), "mail");
        return;
    }
    Logger::instance().info(
        QString("pollAccount() 触发: [%1]").arg(acc->email), "mail");
    startWorker(*acc);
}

void MailPoller::onTimerTick() {
    pollNow();
}

void MailPoller::onWorkerFinished(const QString& accountId) {
    m_workers.remove(accountId);
    m_lastRefreshTime = QDateTime::currentDateTime();
    Logger::instance().info(
        QString("Worker 完成: accountId=%1 (剩余 %2, 上次刷新=%3)")
            .arg(accountId)
            .arg(m_workers.size())
            .arg(m_lastRefreshTime.toString("HH:mm:ss")),
        "mail");
    emit accountPolled(accountId);
}

bool MailPoller::startWorker(const MailAccountManager::Account& a) {
    if (a.recvProto == "SMTP") {
        Logger::instance().info(
            QString("跳过账号 [%1]: SMTP-only").arg(a.email), "mail");
        return false;
    }
    if (a.imapHost.isEmpty()) {
        Logger::instance().warn(
            QString("跳过账号 [%1]: 收件服务器为空 (recv=%2)")
                .arg(a.email, a.recvProto), "mail");
        return false;
    }
    if (a.password.isEmpty()) {
        Logger::instance().warn(
            QString("跳过账号 [%1]: 密码为空").arg(a.email), "mail");
        return false;
    }
    if (m_workers.contains(a.id)) {
        Logger::instance().info(
            QString("跳过账号 [%1]: worker 已在运行").arg(a.email), "mail");
        return false;
    }

    auto* thread = new QThread(this);
    auto* worker = new MailWorker(a.id);
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &MailWorker::run);
    connect(worker, &MailWorker::finished, this, &MailPoller::onWorkerFinished);
    connect(worker, &MailWorker::notify, this, &MailPoller::notify);
    connect(worker, &MailWorker::finished, thread, &QThread::quit);
    connect(worker, &MailWorker::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    Worker w;
    w.thread    = thread;
    w.accountId = a.id;
    m_workers.insert(a.id, w);
    thread->start();
    Logger::instance().info(
        QString("Worker 启动: [%1] -> %2:%3 ssl=%4")
            .arg(a.email, a.imapHost).arg(a.imapPort).arg(a.imapSsl ? "Y" : "N"),
        "mail");
    return true;
}

// ── 全量同步（阶段① LIST → 阶段② 每文件夹一线程 fetchHeaders）────────────

void MailPoller::startFullSyncCycle(int intervalMin) {
    if (intervalMin < 1) intervalMin = 1;
    m_fullSyncTimer.start(intervalMin * 60 * 1000);
    Logger::instance().info(
        QString("邮件全量同步定时器已启动，间隔 %1 分钟").arg(intervalMin), "mail");
}

void MailPoller::stopFullSyncCycle() {
    m_fullSyncTimer.stop();
    // 让所有运行中的阶段①/阶段② 回调因 round mismatch 被丢弃，
    // 解决"stop 后又开新一轮"导致的回调错位与计数错乱。
    ++m_fullSyncRound;
    m_fullSyncActive = false;
}

void MailPoller::onFullSyncTimerTick() {
    runFullSyncNow();
}

void MailPoller::runFullSyncNow() {
    if (m_fullSyncActive) {
        Logger::instance().info("全量同步已在进行中，忽略重复触发", "mail");
        return;
    }
    beginFullSyncRound();
}

void MailPoller::beginFullSyncRound() {
    const auto& accounts = MailAccountManager::instance().accounts();
    QStringList syncable;
    for (const auto& a : accounts) {
        if (a.recvProto == "SMTP" || a.imapHost.isEmpty() || a.password.isEmpty())
            continue;
        syncable.append(a.id);
    }
    if (syncable.isEmpty()) {
        Logger::instance().info("全量同步: 无可同步账号", "mail");
        return;
    }

    m_fullSyncActive = true;
    const int round = ++m_fullSyncRound;
    m_fullSyncPendingAccounts = syncable.size();
    m_fullSyncFoldersTotal    = 0;
    m_fullSyncFoldersDone     = 0;
    m_fullSyncFoldersSucceeded = 0;
    m_fullSyncFolders.clear();

    Logger::instance().info(
        QString("══ 全量同步开始：阶段① LIST 文件夹（%1 个账号） ══")
            .arg(syncable.size()),
        "mail");
    emit fullSyncStarted(syncable.size());

    // 阶段①：每个账号一个独立线程跑 IMAP LIST
    for (const QString& accId : syncable) {
        auto* acc = MailAccountManager::instance().getById(accId);
        if (!acc) {
            onAccountListDone(round, accId, false, {}, "账号不存在");
            continue;
        }
        ImapClient::Config cfg;
        cfg.host       = acc->imapHost;
        cfg.port       = acc->imapPort;
        cfg.ssl        = acc->imapSsl;
        cfg.username   = acc->email;
        cfg.password   = acc->password;
        cfg.timeoutSec = 60;

        QThread* t = QThread::create([this, round, accId, cfg]() {
            QList<ImapClient::Folder> folders;
            QString err;
            bool ok = ImapClient::listFolders(cfg, &folders, &err);
            QMetaObject::invokeMethod(this,
                [this, round, accId, ok, folders, err]() {
                    onAccountListDone(round, accId, ok, folders, err);
                },
                Qt::QueuedConnection);
        });
        connect(t, &QThread::finished, t, &QObject::deleteLater);
        t->start();
    }
}

void MailPoller::onAccountListDone(int round, const QString& accountId,
                                   bool ok,
                                   const QList<ImapClient::Folder>& folders,
                                   const QString& err) {
    if (round != m_fullSyncRound) return;   // 过期轮次：丢弃

    if (ok) {
        m_fullSyncFolders.insert(accountId, folders);
        Logger::instance().info(
            QString("全量同步 阶段①: 账号 [%1] 文件夹 %2 个")
                .arg(accountId).arg(folders.size()),
            "mail");
    } else {
        Logger::instance().warn(
            QString("全量同步 阶段①: 账号 [%1] LIST 失败: %2")
                .arg(accountId, err),
            "mail");
    }

    if (--m_fullSyncPendingAccounts > 0) return;

    // 阶段① 全部完成 → 进入阶段②
    struct Job { QString acc; QString folder; };
    QList<Job> jobs;
    for (auto it = m_fullSyncFolders.begin(); it != m_fullSyncFolders.end(); ++it) {
        const QString& accId = it.key();
        const auto& folders  = it.value();
        QString inboxName, sentName;
        for (const auto& f : folders) {
            if (f.flags.contains("\\Noselect", Qt::CaseInsensitive)) continue;
            QString low = ImapClient::decodeFolderName(f.name).toLower();
            if (low == "inbox") {
                inboxName = f.name;
            } else if (sentName.isEmpty()
                       && (low.contains("sent") || low.contains("已发送"))) {
                sentName = f.name;
            }
        }
        if (!inboxName.isEmpty()) jobs.append({accId, inboxName});
        if (!sentName.isEmpty())  jobs.append({accId, sentName});
        // 其余 \Noselect 跳过；inbox/sent/draft 跳过（draft 本地管理，不拉服务器）
        for (const auto& f : folders) {
            if (f.flags.contains("\\Noselect", Qt::CaseInsensitive)) continue;
            QString low = ImapClient::decodeFolderName(f.name).toLower();
            if (low == "inbox" || low.contains("sent") || low.contains("draft"))
                continue;
            jobs.append({accId, f.name});
        }
    }

    if (jobs.isEmpty()) {
        Logger::instance().info("全量同步: 没有可同步的文件夹", "mail");
        m_fullSyncActive = false;
        emit fullSyncFinished(0, 0);
        return;
    }

    m_fullSyncFoldersTotal = jobs.size();
    Logger::instance().info(
        QString("══ 全量同步 阶段②：每文件夹一线程并行拉取，共 %1 个文件夹（并发上限 3） ══")
            .arg(jobs.size()),
        "mail");
    emit fullSyncFoldersListed(jobs.size());

    // 阶段②：每个 (账号, 文件夹) 一个独立线程，全局 m_folderSem 限流到 3
    for (const auto& j : jobs) {
        auto* acc = MailAccountManager::instance().getById(j.acc);
        if (!acc) {
            onFolderSyncDone(round, j.acc, j.folder, false, "账号不存在");
            continue;
        }
        ImapClient::Config cfg;
        cfg.host       = acc->imapHost;
        cfg.port       = acc->imapPort;
        cfg.ssl        = acc->imapSsl;
        cfg.username   = acc->email;
        cfg.password   = acc->password;
        cfg.timeoutSec = 60;

        QThread* t = QThread::create([this, round, cfg, j]() {
            m_folderSem.acquire();
            QList<ImapClient::FetchedMessage> fetched;
            QString err;
            bool ok = ImapClient::fetchHeaders(cfg, j.folder, /*maxCount*/50,
                                               &fetched, &err);
            // 在后台线程构造 MailStore::Message，回主线程统一 upsert（MailStore 非线程安全）
            QList<MailStore::Message> toUpsert;
            if (ok) {
                for (const auto& f : fetched) {
                    MailStore::Message m;
                    m.accountId = j.acc;
                    m.folder    = j.folder;
                    m.messageId = f.messageId;
                    m.imapUid   = f.imapUid;
                    m.from      = f.from;
                    m.to        = f.to;
                    m.cc        = f.cc;
                    m.subject   = f.subject;
                    m.date      = f.date;
                    m.read      = f.seen;
                    toUpsert.append(m);
                }
            }
            QMetaObject::invokeMethod(this,
                [this, round, j, ok, toUpsert, err]() {
                    if (!toUpsert.isEmpty())
                        MailStore::instance().upsertMessages(toUpsert);
                    onFolderSyncDone(round, j.acc, j.folder, ok, err);
                },
                Qt::QueuedConnection);
            m_folderSem.release();
        });
        connect(t, &QThread::finished, t, &QObject::deleteLater);
        t->start();
    }
}

void MailPoller::onFolderSyncDone(int round, const QString& accountId,
                                  const QString& folder, bool ok,
                                  const QString& err) {
    if (round != m_fullSyncRound) return;   // 过期轮次：丢弃

    if (ok) {
        ++m_fullSyncFoldersSucceeded;
        Logger::instance().info(
            QString("全量同步 阶段②: [%1|%2] ✓").arg(accountId, folder),
            "mail");
    } else {
        Logger::instance().warn(
            QString("全量同步 阶段②: [%1|%2] ✗ %3").arg(accountId, folder, err),
            "mail");
    }

    if (++m_fullSyncFoldersDone < m_fullSyncFoldersTotal) return;

    // 全部完成
    Logger::instance().info(
        QString("══ 全量同步完成：%1/%2 个文件夹成功 ══")
            .arg(m_fullSyncFoldersSucceeded).arg(m_fullSyncFoldersTotal),
        "mail");
    m_fullSyncActive = false;
    emit fullSyncFinished(m_fullSyncFoldersSucceeded, m_fullSyncFoldersTotal);
}

// ── MailWorker（INBOX 轮询，原逻辑保留）──────────────────────────────────

MailWorker::MailWorker(QString accountId, QObject* parent)
    : QObject(parent), m_accountId(std::move(accountId)) {}

void MailWorker::run() {
    auto* acc = MailAccountManager::instance().getById(m_accountId);
    if (!acc) {
        Logger::instance().warn(
            QString("Worker.run() 账号不存在: id=%1").arg(m_accountId), "mail");
        emit finished(m_accountId);
        return;
    }

    Logger::instance().info(
        QString("Worker.run() 开始: [%1] -> %2:%3 ssl=%4 user=%5")
            .arg(acc->email, acc->imapHost).arg(acc->imapPort)
            .arg(acc->imapSsl ? "Y" : "N").arg(acc->email),
        "mail");

    ImapClient::Config cfg;
    cfg.host     = acc->imapHost;
    cfg.port     = acc->imapPort;
    cfg.ssl      = acc->imapSsl;
    cfg.username = acc->email;
    cfg.password = acc->password;
    cfg.timeoutSec = 120;

    QList<ImapClient::FetchedMessage> fetched;
    QString err;
    // ENVELOPE 批量拉取不受 139 对 BODY[...] 的限流影响，可放宽到 30 封
    if (!ImapClient::fetchUnread(cfg, &fetched, &err, /*maxCount*/ 30)) {
        Logger::instance().warn(
            QString("IMAP 拉取失败 [%1]: %2").arg(acc->email, err), "mail");
        emit finished(m_accountId);
        return;
    }
    Logger::instance().info(
        QString("IMAP ENVELOPE 完成: %1 封 — 开始逐封同步拉正文")
            .arg(fetched.size()), "mail");

    // 转换为 MailStore::Message
    QList<MailStore::Message> toUpsert;
    for (const auto& f : fetched) {
        MailStore::Message m;
        m.accountId = acc->id;
        m.folder    = "INBOX";
        m.messageId = f.messageId;
        m.imapUid   = f.imapUid;
        m.from      = f.from;
        m.to        = f.to;
        m.cc        = f.cc;
        m.subject   = f.subject;
        m.body      = f.body;
        m.date      = f.date;
        m.read      = f.seen;   // 服务器 FLAGS 的 \Seen 状态
        toUpsert.append(m);
    }

    // 阶段 2：逐封 fetchBody 同步拉正文（body + html + 附件一起；
    // 139 对 batch BODY.PEEK 严格限流，只能单封串行，间隔 2 秒友好限速）。
    // 失败单封不阻塞其它邮件。
    int bodyOk = 0, bodyFail = 0;
    for (int i = 0; i < toUpsert.size(); ++i) {
        auto& m = toUpsert[i];
        if (m.imapUid.isEmpty()) continue;
        QString body, html, fetchErr;
        QByteArray raw;
        QList<MailStore::Attachment> atts;
        if (ImapClient::fetchBody(cfg, "INBOX", m.imapUid,
                                  &body, &html, &raw, &atts, &fetchErr)) {
            m.body        = body;
            m.htmlBody    = html;
            m.rawSource   = raw;
            m.attachments = atts;
            ++bodyOk;
        } else {
            ++bodyFail;
            Logger::instance().warn(
                QString("IMAP BODY 拉取失败 [uid=%1]: %2 — 留空，列表摘要不显示")
                    .arg(m.imapUid).arg(fetchErr), "mail");
        }
        if (i + 1 < toUpsert.size()) QThread::msleep(2000);
    }
    Logger::instance().info(
        QString("IMAP 同步正文: 成功 %1/%2 (失败 %3)")
            .arg(bodyOk).arg(toUpsert.size()).arg(bodyFail), "mail");

    if (!toUpsert.isEmpty()) {
        // MailStore 非线程安全：回主线程写（MailStore 对象驻留主线程）
        QMetaObject::invokeMethod(&MailStore::instance(), [toUpsert]() {
            MailStore::instance().upsertMessages(toUpsert);
        }, Qt::QueuedConnection);
        // 通知 UI（信号跨线程自动排队到主线程）
        for (const auto& m : toUpsert) {
            emit notify(acc->name, m.subject, m.from);
        }
    }
    emit finished(m_accountId);
}