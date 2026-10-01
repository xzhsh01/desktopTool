#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QList>

/**
 * WeChatWorker — 后台线程统一执行微信耗时 IO
 *
 * 三个职责：
 *   1) 加载账号的会话 + 联系人（解密 db + 读两张表）
 *   2) 加载某个 talker 的最近 N 条消息
 *   3) 从运行中的微信进程提取 DB 密钥
 *
 * 用法：owner ctor 里 new WeChatWorker + new QThread，moveToThread + start；
 *       owner 析构里 quit() + wait()，再 deleteLater worker。
 *
 * 跨线程调用规则：
 *   - 用 QMetaObject::invokeMethod(..., Qt::QueuedConnection) 或 signal/slot
 *     触发本 worker 的 slot
 *   - worker 完成后 emit 信号，owner 在主线程接收并更新 UI
 */
class WeChatWorker : public QObject {
    Q_OBJECT
public:
    explicit WeChatWorker(QObject* parent = nullptr);

public slots:
    // 加载账号的会话 + 联系人（一次性，懒加载 + 缓存友好）
    void loadAccountData(const QString& accId);

    // 加载某个 talker 的最近 N 条消息
    void loadMessages(const QString& accId,
                      const QString& talker,
                      int limit = 500);

    // 从正在运行的微信进程提取 DB 密钥
    void extractKey(const QString& dbPath);

    // 从正在运行的微信进程提取 V2 图片 AES-128-ECB key（需要已知 .dat 文件做 oracle）
    // （可选附加任务）从指定 pid 再尝试找 db key + image key（用于后台扫描）
    void extractImageKey(const QString& knownDatPath);

signals:
    // 加载账号数据
    void accountLoaded(const QString& accId,
                       const QVariantList& sessions,
                       const QVariantList& contacts);
    void accountFailed(const QString& accId, const QString& reason);

    // 加载消息
    void messagesLoaded(const QString& accId,
                        const QString& talker,
                        const QString& title,
                        const QList<QVariantMap>& messages);
    void messagesFailed(const QString& accId,
                        const QString& talker,
                        const QString& reason);

    // 提取密钥
    void keyExtracted(const QString& key, const QString& err);
    // 提取图片 key
    void imageKeyExtracted(const QString& key16Hex, const QString& err);
};