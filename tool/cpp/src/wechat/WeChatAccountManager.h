#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QVariantMap>
#include <QDateTime>

/**
 * WeChatAccountManager: 微信账号管理（支持多账号）
 *
 * 本机可同时登录多个微信（多个微信进程 / 多个数据目录），
 * 每个账号对应一个本地微信数据目录（wxid_xxx）。
 * 数据库密钥使用 DPAPI 加密落盘（复用 Crypto）。
 *
 * 账号发现：
 *   - 微信 3.x: <微信文件目录>\WeChat Files\wxid_*\Msg\MicroMsg.db
 *   - 微信 4.x: <文档目录>\xwechat_files\wxid_*\db_storage
 *   微信文件目录优先读取注册的自定义路径配置
 *   (%APPDATA%\Tencent\WeChat\All Users\config\3ebffe94.ini)
 */
class WeChatAccountManager : public QObject {
    Q_OBJECT

public:
    struct Account {
        QString id;          // 内部唯一 id
        QString name;        // 显示名称（昵称/备注）
        QString wxid;        // 微信 id（wxid_xxx）
        QString dataDir;     // 该账号的数据目录（.../WeChat Files/wxid_xxx）
        QString keyHex;      // 数据库解密密钥（64 位 hex，DPAPI 加密存储）
        QString version;     // "3.x" / "4.x"
        QDateTime createdAt;
        QDateTime updatedAt;
    };

    // 扫描到的本机账号（未配置密钥）
    struct DiscoveredAccount {
        QString wxid;
        QString dataDir;
        QString version;     // "3.x" / "4.x"
        QString nickname;   // 尽力从 acc_info.dat 等读取，可能为空
    };

    static WeChatAccountManager& instance();

    const QList<Account>& accounts() const { return m_accounts; }
    Account* getById(const QString& id);
    Account* getByWxid(const QString& wxid);

    // 新增/更新（data 为 Account 字段；keyHex 传明文 hex，内部加密存储）
    Account* add(const QVariantMap& data);
    bool update(const QString& id, const QVariantMap& data);
    void remove(const QString& id);

    // 账号密钥（DPAPI 解密后的明文 hex）
    QString keyForAccount(const Account& acc) const;

    // 扫描本机微信数据目录，返回发现的账号列表
    QList<DiscoveredAccount> discoverLocalAccounts() const;

    // 微信数据根目录（WeChat Files 所在目录），供配置界面默认值
    static QString defaultWeChatFilesRoot();
    // 当前用户「文档」目录
    static QString documentsDir();

signals:
    void changed();   // 账号列表发生变化（增删改）

private:
    WeChatAccountManager(QObject* parent = nullptr);
    WeChatAccountManager(const WeChatAccountManager&) = delete;
    WeChatAccountManager& operator=(const WeChatAccountManager&) = delete;

    void load();
    void save();

    QList<Account> m_accounts;
    QString m_filePath;
};
