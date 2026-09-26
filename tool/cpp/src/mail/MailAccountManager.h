#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QVariantMap>
#include <QDateTime>
#include <QUuid>

/**
 * MailAccountManager: 邮箱账号管理（多账号）
 *
 * - 账号以 JSON 列表形式存储于 %APPDATA%/KFrame/bambooRat/mail_accounts.json
 * - 密码用 DPAPI 加密（与 ConnectionManager 一致）
 * - 单例 + 信号 accountsChanged
 */
class MailAccountManager : public QObject {
    Q_OBJECT

public:
    struct Account {
        QString id;              // UUID
        QString name;            // 昵称（仅 UI 用，如 "工作邮箱"）
        QString email;           // 完整邮箱地址（同时作为 SMTP/IMAP username）
        QString displayName;     // 发件人显示名（可选）

        // 发件 (SMTP)
        QString smtpHost;
        int     smtpPort = 465;
        bool    smtpSsl = true;  // true=SSL 直连，false=STARTTLS

        // 收件（IMAP / POP3 二选一）
        // 注意：imapHost/imapPort/imapSsl 字段名保留以兼容老 JSON，
        // 语义上表示"收件服务器"的实际主机/端口/SSL，不区分协议。
        QString recvProto = "IMAP"; // "IMAP"、"POP3" 或 "SMTP"（仅发件，不收件）
        QString imapHost;            // 实际收件主机（imap.xxx 或 pop.xxx）——当前协议主机
        int     imapPort = 993;
        bool    imapSsl = true;
        // POP3 主机独立保存，方便在协议间切换而不丢数据
        QString pop3Host;
        int     pop3Port = 995;
        bool    pop3Ssl = true;

        // CalDAV（可选，iCloud / Outlook 等日历/联系人同步）
        bool    calDavEnabled = false;
        QString calDavHost;
        int     calDavPort = 443;
        bool    calDavSsl = true;
        QString calDavUser;          // 默认 = email

        QString password;        // 内存中明文；落盘时加密
        bool    isDefault = false;
        QDateTime createdAt;
        QDateTime updatedAt;

        // IMAP LIST 结果缓存（QVariantMap{name,delimiter,flags} 列表）：
        // 启动时先显示上次同步到的全部文件夹，再由后台 LIST 覆盖刷新
        QVariantList cachedFolders;
    };

    static MailAccountManager& instance();

    void load();
    void save();

    const QList<Account>& accounts() const { return m_accounts; }
    Account* getById(const QString& id);
    Account* getDefault();

    // data 必须包含 email；其他字段可选（缺失时使用默认值）
    Account add(const QVariantMap& data);
    bool update(const QString& id, const QVariantMap& data);
    bool remove(const QString& id);
    void setDefault(const QString& id);
    // 持久化 IMAP 文件夹列表缓存（loadRemoteFolders 成功后调用；仅落盘，不触发 accountsChanged）
    void setCachedFolders(const QString& accountId, const QVariantList& folders);

    // 工具：QQ/163/Gmail/Outlook/Exchange 等常见邮箱预设
    static QVariantMap presetFor(const QString& email);

signals:
    void accountsChanged();

private:
    MailAccountManager(QObject* parent = nullptr);
    MailAccountManager(const MailAccountManager&) = delete;
    MailAccountManager& operator=(const MailAccountManager&) = delete;

    QList<Account> m_accounts;
    QString m_filePath;

    static constexpr const char* ENC_PREFIX = "enc:";
};
