#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QDateTime>
#include <QVariantMap>

/**
 * WeChatDb: 微信本地数据库解密与读取
 *
 * 微信 3.x 数据库为 SQLCipher 格式（AES-256-CBC + HMAC-SHA1）：
 *   - 文件前 16 字节为 salt
 *   - aes_key  = PBKDF2-HMAC-SHA1(用户密钥, salt, 64000, 32)
 *   - mac_key  = PBKDF2-HMAC-SHA1(aes_key, salt^0x58, 2, 32)
 *   - 每页 4096 字节：[加密内容][IV 16][HMAC-SHA1 20][保留 12]
 *   - 首页密文区从偏移 16 开始（salt 之后）
 *
 * 解密后输出标准 SQLite 文件到缓存目录，再用内置 sqlite3 读取：
 *   - MicroMsg.db: Session(会话) / Contact(联系人) / ChatRoom(群聊)
 *   - MSG*.db:     MSG(聊天记录)
 */
class WeChatDb {
public:
    // ── 数据结构 ──────────────────────────────────────────────
    struct ChatSession {
        QString talker;      // 会话对方 wxid / 群聊 id
        QString title;       // 显示名称（备注 > 昵称 > 群名 > wxid）
        QString lastMsg;     // 最后一条消息预览
        QDateTime lastTime;  // 最后消息时间
        int unread = 0;      // 未读数
        bool isChatRoom = false;
    };

    struct ChatMessage {
        qint64 msgId = 0;
        QString talker;       // 所属会话
        QString senderId;     // 发送者（群聊内成员）
        QString senderName;   // 发送者显示名
        bool isSender = false; // 是否自己发送
        int type = 1;         // 微信消息类型
        int subType = 0;
        QString content;      // 文本内容 / 系统消息
        QDateTime time;
        QString display;      // 渲染用描述（[图片] [语音] 等）
        // 附件元信息（type=49 子类型、3/34/43/47/48 等）
        QString attachTitle;  // 文件名 / 链接标题
        qint64 attachSize = 0;
        QString attachExt;    // 扩展名 / 类型标记
        QString attachUrl;    // 链接 / 下载 URL
        QString attachMime;   // 消息子类型文本（appmsg/type）
    };

    struct Contact {
        QString userName;
        QString nickname;
        QString remark;
        QString alias;
        int type = 0;
        int verifyFlag = 0;
        QString display;      // 备注 > 昵称 > wxid
        bool isChatRoom = false;
    };

    // 单联系人的详细信息（详情页用；字段比 Contact 多）
    struct ContactDetail : public Contact {
        QString smallHeadUrl;    // 缩略头像 URL
        QString bigHeadUrl;      // 高清头像 URL
        QString signature;       // 个性签名（3.x 直接字段；4.x 从 extra_buffer 解析）
        QString province;        // 省
        QString city;            // 市
        QString country;         // 国
        int sex = 0;             // 0=未知 1=男 2=女
    };

    // ── 解密 ──────────────────────────────────────────────────
    // 校验密钥是否正确（读取首页并做 HMAC 验证，不解密整个文件）
    static bool verifyKey(const QString& keyHex, const QString& dbPath, QString* errOut = nullptr);

    // 解密整个数据库文件到 dstPath（标准 SQLite 文件）
    static bool decryptDatabase(const QString& keyHex, const QString& srcPath,
                                const QString& dstPath, QString* errOut = nullptr);

    // ── 实例：绑定一个账号 ────────────────────────────────────
    // accountId 仅用于缓存目录隔离；dataDir 为 wxid_xxx 目录
    WeChatDb(const QString& accountId, const QString& dataDir, const QString& keyHex);
    ~WeChatDb();

    // 确保 MicroMsg / MSG* 已解密到缓存（源文件更新时自动重新解密）。
    // 返回 false 时错误信息在 lastError()
    bool ensureDecrypted();
    const QString& lastError() const { return m_lastError; }

    // 会话列表（按最后消息时间倒序）
    QList<ChatSession> loadSessions();
    // 某会话的聊天记录（时间升序；limit<=0 表示全部）
    QList<ChatMessage> loadMessages(const QString& talker, int limit = 0);
    // 联系人列表（按显示名排序；包含群聊）
    QList<Contact> loadContacts();
    // 单联系人的详细信息（比 loadContacts 字段多；从 contact.db 直接查询）
    ContactDetail loadContactDetail(const QString& wxid);
    // 群成员 wxid 列表
    QStringList chatRoomMembers(const QString& chatRoomId);
    // 联系人显示名（备注 > 昵称 > wxid），查不到返回 wxid 本身
    QString displayName(const QString& wxid);
    // 账号自己的昵称（MicroMsg.db 内部 wxid 对应的联系人）
    QString selfDisplayName();

private:
    // 解密单个源库到缓存并返回解密后路径；失败返回空串
    QString cachedDb(const QString& srcPath);
    // 在解密后的 MicroMsg 缓存上执行查询（返回行列表）
    QList<QVariantList> queryMicro(const QString& sql, const QStringList& args);
    // 在所有解密后的 MSG 缓存上执行查询（结果合并）
    QList<QVariantList> queryMsg(const QString& sql, const QStringList& args);
    // 检测微信版本（3.x → 3；4.x → 4；否则 0）
    int detectVersion();
    // 各类型数据库的解密缓存路径（自动选择 3.x / 4.x 路径）
    QString contactDbCache();
    QString sessionDbCache();
    QStringList msgDbCaches();
    // 在指定解密库上执行查询
    QList<QVariantList> runOn(const QString& dbPath, const QString& sql,
                              const QStringList& args);

    QString m_accountId;
    QString m_dataDir;
    QString m_keyHex;
    QString m_cacheDir;
    QString m_lastError;
    int m_version = 0;            // 3 / 4 / 0=未识别

    // wxid -> 显示名 缓存
    QVariantMap m_nameCache;
    bool m_namesLoaded = false;
};
