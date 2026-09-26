#pragma once

#include <QString>
#include <QStringList>
#include <QList>
#include <QDateTime>

#include "wechat/WeChatDb.h"

/**
 * WeChatBackup: 聊天记录持久备份（每账号一个 SQLite 库）
 *
 * 目标：微信里删除了聊天记录，本程序仍能找回历史。
 *   - 备份库存放在程序数据目录：<AppData>/wechat_backup/<accountId>.db
 *   - 与解密缓存（wechat_cache）不同：缓存是微信源库的镜像，
 *     源库更新后会被覆盖，微信删了记录缓存里也就没了；
 *     备份库「只增不删」——只要同步过的消息永远保留。
 *   - 消息按 (talker, local_key, time) 去重：
 *       v4 local_key = sort_seq；v3 local_key = localId
 *   - 打开会话时：先同步微信当前消息进备份，再从备份读全部历史，
 *     因此显示的永远是「有史以来见过的所有消息」。
 *
 * 线程安全：全量备份与打开会话的增量同步可能并发（不同线程），
 * 所有操作经全局互斥锁串行化；SQLite 侧另设 busy_timeout 兜底。
 */
class WeChatBackup {
public:
    explicit WeChatBackup(const QString& accountId);
    ~WeChatBackup();

    // 打开/建库（含 schema）。失败时 lastError 有信息
    bool open();
    const QString& lastError() const { return m_lastError; }

    // 同步一批消息进备份（INSERT OR IGNORE，只增不删）。
    // 返回本次新插入的条数；失败返回 -1
    qint64 syncMessages(const QList<WeChatDb::ChatMessage>& msgs);

    // 某会话的全部历史（时间升序，含微信端已删除的消息）
    QList<WeChatDb::ChatMessage> messages(const QString& talker);

    // 搜索消息：content/display 模糊匹配（大小写不敏感），时间倒序。
    // talker 为空 = 全账号所有会话；limit 限制结果数
    QList<WeChatDb::ChatMessage> searchMessages(const QString& keyword,
                                                const QString& talker = QString(),
                                                int limit = 200);

    // 附件类消息（图片/语音/视频/动画表情/文件），时间倒序。
    // talker 为空 = 所有会话；limit 限制结果数
    QList<WeChatDb::ChatMessage> attachmentMessages(const QString& talker = QString(),
                                                    int limit = 1000);

    // 备份库消息总数（供 UI 显示「已备份 N 条」）
    qint64 messageCount();

    // 备份库文件路径（调试/诊断用）
    static QString backupPath(const QString& accountId);

private:
    bool exec(const QString& sql);

    QString m_accountId;
    QString m_path;
    QString m_lastError;
    void*  m_db = nullptr;   // sqlite3*（避免头文件污染）
};
