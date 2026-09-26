#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ScheduledQueue: 定时发送队列（单例）
//
//   - 落盘到 <AppDataLocation>/mail/scheduled/<id>.json
//   - 每 30 秒扫描一次，到点调 SmtpClient::send；成功后删除 JSON 文件
//   - 不影响 UI 主线程：所有磁盘 / SMTP I/O 都在 onTimer 触发（仍在主线程，
//     但与 UI 点击响应错开；可扩展到 QThreadPool 子线程）
//
// 关联信号：
//   - queueChanged()     UI 刷新"定时列表"
//   - fired(id)          一封已成功发出
//   - failed(id, err)    一封发送失败（保留文件以便用户重试 / 取消）
// ─────────────────────────────────────────────────────────────────────────────

#include <QObject>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QList>
#include <QDateTime>
#include <QSet>

class ScheduledQueue : public QObject {
    Q_OBJECT
public:
    // 单附件 / 单 inline 元数据（仅 disk 路径 + 显示名 + cid）
    struct AttMeta  { QString filePath; QString displayName; };
    struct InlineMeta { QString filePath; QString displayName; QString contentId; };

    struct Item {
        QString id;             // UUID（无花括号）
        QDateTime fireAt;       // 触发时间（本地时区，到点扫描）
        QString accountId;      // 发件账号
        QStringList to, cc, bcc;
        QString subject;
        QString body;           // 纯文本 fallback
        QString htmlBody;       // 富文本正文（可空）
        QList<AttMeta> attachments;
        QList<InlineMeta> inlineImages;
        bool urgent      = false;
        bool readReceipt = false;
        QString inReplyTo;
        QStringList references;
    };

    static ScheduledQueue& instance();

    // 调度一个新任务；返回 id（即 Item.id）。
    QString schedule(Item it);

    // 按 id 取消；返回是否找到。
    bool cancel(const QString& id);

    // 列表 / 数量
    QList<Item> listAll() const { return m_items; }
    int  pendingCount() const;

    // 把内存状态重新写盘（用于调试或手动调用）
    void flushAll();

signals:
    void queueChanged();
    void fired(const QString& id);
    void failed(const QString& id, const QString& error);

private:
    ScheduledQueue();
    QString dirPath() const;
    QString filePath(const QString& id) const;
    void loadFromDisk();
    void onTimer();
    // 真正发送某条；返回是否成功（成功时会从 m_items 移除并删 JSON）
    void fireOne(const Item& it);

    QList<Item> m_items;
    QTimer      m_timer;
    // 防止同 id 在一次 onTimer 内被 fire 两次（理论不会，但更稳）
    QSet<QString> m_inflight;
};