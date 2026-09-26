#include "mail/MailAccountManager.h"
#include "mail/MailStore.h"
#include "core/Crypto.h"
#include "core/Logger.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>

MailAccountManager& MailAccountManager::instance() {
    static MailAccountManager mgr;
    return mgr;
}

MailAccountManager::MailAccountManager(QObject* parent) : QObject(parent) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    m_filePath = dir + "/mail_accounts.json";
    load();
    Logger::instance().info(
        QString("MailAccountManager 初始化: accounts=%1, file=%2")
            .arg(m_accounts.size()).arg(m_filePath),
        "mail");
}

void MailAccountManager::load() {
    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly)) return;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return;

    m_accounts.clear();
    for (const auto& v : doc.array()) {
        QJsonObject o = v.toObject();
        Account a;
        a.id          = o["id"].toString();
        a.name        = o["name"].toString();
        a.email       = o["email"].toString();
        a.displayName = o["displayName"].toString();
        a.smtpHost    = o["smtpHost"].toString();
        a.smtpPort    = o["smtpPort"].toInt(465);
        a.smtpSsl     = o["smtpSsl"].toBool(true);
        {
            QString p = o["recvProto"].toString("IMAP").toUpper();
            a.recvProto = (p == "POP3") ? "POP3" : (p == "SMTP" ? "SMTP" : "IMAP");
        }
        a.imapHost    = o["imapHost"].toString();
        a.imapPort    = o["imapPort"].toInt(993);
        a.imapSsl     = o["imapSsl"].toBool(true);
        a.pop3Host    = o["pop3Host"].toString();
        a.pop3Port    = o["pop3Port"].toInt(995);
        a.pop3Ssl     = o["pop3Ssl"].toBool(true);
        a.calDavEnabled = o["calDavEnabled"].toBool(false);
        a.calDavHost    = o["calDavHost"].toString();
        a.calDavPort    = o["calDavPort"].toInt(443);
        a.calDavSsl     = o["calDavSsl"].toBool(true);
        a.calDavUser    = o["calDavUser"].toString();

        QString storedPass = o["password"].toString();
        if (storedPass.startsWith(ENC_PREFIX)) {
            a.password = Crypto::instance().decrypt(storedPass.mid(qstrlen(ENC_PREFIX)));
        } else {
            a.password = storedPass; // 兼容历史明文
        }

        a.isDefault  = o["isDefault"].toBool(false);
        a.createdAt  = QDateTime::fromString(o["createdAt"].toString(), Qt::ISODate);
        a.updatedAt  = QDateTime::fromString(o["updatedAt"].toString(), Qt::ISODate);
        a.cachedFolders = o["cachedFolders"].toArray().toVariantList();
        m_accounts.append(a);
    }
}

void MailAccountManager::save() {
    QJsonArray arr;
    for (const auto& a : m_accounts) {
        QJsonObject o;
        o["id"]          = a.id;
        o["name"]        = a.name;
        o["email"]       = a.email;
        o["displayName"] = a.displayName;
        o["smtpHost"]    = a.smtpHost;
        o["smtpPort"]    = a.smtpPort;
        o["smtpSsl"]     = a.smtpSsl;
        o["recvProto"]   = a.recvProto;
        o["imapHost"]    = a.imapHost;
        o["imapPort"]    = a.imapPort;
        o["imapSsl"]     = a.imapSsl;
        o["pop3Host"]    = a.pop3Host;
        o["pop3Port"]    = a.pop3Port;
        o["pop3Ssl"]     = a.pop3Ssl;
        o["calDavEnabled"] = a.calDavEnabled;
        o["calDavHost"]    = a.calDavHost;
        o["calDavPort"]    = a.calDavPort;
        o["calDavSsl"]     = a.calDavSsl;
        o["calDavUser"]    = a.calDavUser;
        if (!a.password.isEmpty()) {
            QString enc = Crypto::instance().encrypt(a.password);
            if (!enc.isEmpty()) {
                o["password"] = QString(ENC_PREFIX) + enc;
            }
        }
        o["isDefault"]   = a.isDefault;
        o["createdAt"]   = a.createdAt.toString(Qt::ISODate);
        o["updatedAt"]   = a.updatedAt.toString(Qt::ISODate);
        o["cachedFolders"] = QJsonArray::fromVariantList(a.cachedFolders);
        arr.append(o);
    }

    QFile f(m_filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

MailAccountManager::Account* MailAccountManager::getById(const QString& id) {
    for (auto& a : m_accounts) if (a.id == id) return &a;
    return nullptr;
}

MailAccountManager::Account* MailAccountManager::getDefault() {
    for (auto& a : m_accounts) if (a.isDefault) return &a;
    if (!m_accounts.isEmpty()) return &m_accounts.first();
    return nullptr;
}

MailAccountManager::Account MailAccountManager::add(const QVariantMap& data) {
    Account a;
    a.id          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    a.name        = data.value("name").toString();
    a.email       = data.value("email").toString().trimmed();
    a.displayName = data.value("displayName").toString();
    a.smtpHost    = data.value("smtpHost").toString();
    a.smtpPort    = data.value("smtpPort", 465).toInt();
    a.smtpSsl     = data.value("smtpSsl", true).toBool();
    {
        QString p = data.value("recvProto", a.recvProto).toString().toUpper();
        a.recvProto = (p == "POP3") ? "POP3" : (p == "SMTP" ? "SMTP" : "IMAP");
    }
    a.imapHost    = data.value("imapHost").toString();
    a.imapPort    = data.value("imapPort", 993).toInt();
    a.imapSsl     = data.value("imapSsl", true).toBool();
    a.pop3Host    = data.value("pop3Host").toString();
    a.pop3Port    = data.value("pop3Port", 995).toInt();
    a.pop3Ssl     = data.value("pop3Ssl", true).toBool();
    a.calDavEnabled = data.value("calDavEnabled", false).toBool();
    a.calDavHost    = data.value("calDavHost").toString();
    a.calDavPort    = data.value("calDavPort", 443).toInt();
    a.calDavSsl     = data.value("calDavSsl", true).toBool();
    a.calDavUser    = data.value("calDavUser").toString();
    a.password    = data.value("password").toString();
    a.isDefault   = data.value("isDefault", m_accounts.isEmpty()).toBool();
    a.createdAt   = QDateTime::currentDateTime();
    a.updatedAt   = a.createdAt;
    if (a.name.isEmpty()) a.name = a.email;

    if (a.isDefault) {
        for (auto& x : m_accounts) x.isDefault = false;
    }
    m_accounts.append(a);
    save();
    emit accountsChanged();
    Logger::instance().info(
        QString("账号添加: id=%1 email=%2 recv=%3 host=%4:%5 ssl=%6 default=%7")
            .arg(a.id, a.email, a.recvProto, a.imapHost)
            .arg(a.imapPort).arg(a.imapSsl ? "Y" : "N").arg(a.isDefault ? "Y" : "N"),
        "mail");
    return a;
}

bool MailAccountManager::update(const QString& id, const QVariantMap& data) {
    Account* a = getById(id);
    if (!a) return false;
    if (data.contains("name"))        a->name        = data["name"].toString();
    if (data.contains("email"))       a->email       = data["email"].toString().trimmed();
    if (data.contains("displayName")) a->displayName = data["displayName"].toString();
    if (data.contains("smtpHost"))    a->smtpHost    = data["smtpHost"].toString();
    if (data.contains("smtpPort"))    a->smtpPort    = data["smtpPort"].toInt();
    if (data.contains("smtpSsl"))     a->smtpSsl     = data["smtpSsl"].toBool();
    if (data.contains("recvProto")) {
        QString p = data["recvProto"].toString().toUpper();
        a->recvProto = (p == "POP3") ? "POP3" : (p == "SMTP" ? "SMTP" : "IMAP");
    }
    if (data.contains("imapHost"))    a->imapHost    = data["imapHost"].toString();
    if (data.contains("imapPort"))    a->imapPort    = data["imapPort"].toInt();
    if (data.contains("imapSsl"))     a->imapSsl     = data["imapSsl"].toBool();
    if (data.contains("pop3Host"))    a->pop3Host    = data["pop3Host"].toString();
    if (data.contains("pop3Port"))    a->pop3Port    = data["pop3Port"].toInt();
    if (data.contains("pop3Ssl"))     a->pop3Ssl     = data["pop3Ssl"].toBool();
    if (data.contains("calDavEnabled")) a->calDavEnabled = data["calDavEnabled"].toBool();
    if (data.contains("calDavHost"))    a->calDavHost    = data["calDavHost"].toString();
    if (data.contains("calDavPort"))    a->calDavPort    = data["calDavPort"].toInt();
    if (data.contains("calDavSsl"))     a->calDavSsl     = data["calDavSsl"].toBool();
    if (data.contains("calDavUser"))    a->calDavUser    = data["calDavUser"].toString();
    if (data.contains("password"))    a->password    = data["password"].toString();
    if (data.contains("isDefault"))   a->isDefault   = data["isDefault"].toBool();
    a->updatedAt = QDateTime::currentDateTime();
    if (a->isDefault) {
        for (auto& x : m_accounts) if (x.id != id) x.isDefault = false;
    }
    save();
    emit accountsChanged();
    Logger::instance().info(QString("账号更新: id=%1 email=%2").arg(id, a->email), "mail");
    return true;
}

bool MailAccountManager::remove(const QString& id) {
    for (int i = 0; i < m_accounts.size(); ++i) {
        if (m_accounts[i].id == id) {
            QString email = m_accounts[i].email;
            bool wasDefault = m_accounts[i].isDefault;
            m_accounts.removeAt(i);
            if (wasDefault && !m_accounts.isEmpty()) {
                m_accounts.first().isDefault = true;
            }
            save();
            // 同步清理该账号的本地邮件缓存，避免孤儿数据残留
            MailStore::instance().removeMessagesByAccount(id);
            emit accountsChanged();
            Logger::instance().info(QString("账号删除: id=%1 email=%2").arg(id, email), "mail");
            return true;
        }
    }
    return false;
}

void MailAccountManager::setDefault(const QString& id) {
    for (auto& a : m_accounts) {
        a.isDefault = (a.id == id);
    }
    save();
    emit accountsChanged();
    Logger::instance().info(QString("设为默认账号: id=%1").arg(id), "mail");
}

void MailAccountManager::setCachedFolders(const QString& id, const QVariantList& folders) {
    Account* a = getById(id);
    if (!a) return;
    a->cachedFolders = folders;
    save();   // 仅落盘：文件夹树刷新由调用方负责，避免 accountsChanged 引发整页重建
}

QVariantMap MailAccountManager::presetFor(const QString& email) {
    QVariantMap p;
    QString domain = email.section('@', 1).toLower();
    if (domain == "qq.com") {
        p["smtpHost"] = "smtp.qq.com";       p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.qq.com";  p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "pop.qq.com";       p["pop3Port"] = 995; p["pop3Ssl"] = true;
    } else if (domain == "163.com") {
        p["smtpHost"] = "smtp.163.com";      p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.163.com";  p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "pop.163.com";       p["pop3Port"] = 995; p["pop3Ssl"] = true;
    } else if (domain == "126.com") {
        p["smtpHost"] = "smtp.126.com";      p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.126.com";  p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "pop.126.com";       p["pop3Port"] = 995; p["pop3Ssl"] = true;
    } else if (domain == "gmail.com") {
        p["smtpHost"] = "smtp.gmail.com";    p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.gmail.com"; p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "pop.gmail.com";     p["pop3Port"] = 995; p["pop3Ssl"] = true;
    } else if (domain == "outlook.com" || domain == "hotmail.com" || domain == "live.com") {
        p["smtpHost"] = "smtp.office365.com";p["smtpPort"] = 587; p["smtpSsl"] = false; // STARTTLS
        p["recvProto"] = "IMAP"; p["imapHost"] = "outlook.office365.com"; p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "outlook.office365.com"; p["pop3Port"] = 995; p["pop3Ssl"] = true;
        p["calDavHost"] = "outlook.office365.com"; p["calDavPort"] = 443; p["calDavSsl"] = true;
    } else if (domain == "exmail.qq.com") {
        p["smtpHost"] = "smtp.exmail.qq.com"; p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.exmail.qq.com"; p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "pop.exmail.qq.com"; p["pop3Port"] = 995; p["pop3Ssl"] = true;
    } else if (domain == "aliyun.com") {
        p["smtpHost"] = "smtp.aliyun.com";    p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.aliyun.com"; p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "pop.aliyun.com";    p["pop3Port"] = 995; p["pop3Ssl"] = true;
    } else if (domain == "yahoo.com" || domain == "yahoo.com.cn") {
        p["smtpHost"] = "smtp.mail.yahoo.com";p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.mail.yahoo.com"; p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "pop.mail.yahoo.com"; p["pop3Port"] = 995; p["pop3Ssl"] = true;
    } else if (domain == "icloud.com" || domain == "me.com" || domain == "mac.com") {
        p["smtpHost"] = "smtp.mail.me.com";   p["smtpPort"] = 587; p["smtpSsl"] = false; // STARTTLS
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.mail.me.com"; p["imapPort"] = 993; p["imapSsl"] = true;
        p["calDavHost"] = "caldav.icloud.com"; p["calDavPort"] = 443; p["calDavSsl"] = true;
    } else if (domain == "139.com") {
        // 中国移动 139 邮箱：IMAP 需登录网页版开启「客户端授权」并设置授权码
        p["smtpHost"] = "smtp.139.com";       p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.139.com"; p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "pop.139.com";        p["pop3Port"] = 995; p["pop3Ssl"] = true;
    } else if (domain == "sina.com" || domain == "sina.cn") {
        // 新浪邮箱：需登录网页版开启「客户端授权」
        p["smtpHost"] = "smtp.sina.com";      p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.sina.com"; p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "pop.sina.com";       p["pop3Port"] = 995; p["pop3Ssl"] = true;
    } else if (domain == "sohu.com") {
        p["smtpHost"] = "smtp.sohu.com";      p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.sohu.com"; p["imapPort"] = 993; p["imapSsl"] = true;
    } else if (domain == "21cn.com") {
        p["smtpHost"] = "smtp.21cn.com";      p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.21cn.com"; p["imapPort"] = 993; p["imapSsl"] = true;
    } else if (domain == "yeah.net") {
        p["smtpHost"] = "smtp.yeah.net";      p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.yeah.net"; p["imapPort"] = 993; p["imapSsl"] = true;
    } else if (domain == "foxmail.com") {
        // Foxmail 与 QQ 邮箱同套后端
        p["smtpHost"] = "smtp.qq.com";        p["smtpPort"] = 465; p["smtpSsl"] = true;
        p["recvProto"] = "IMAP"; p["imapHost"] = "imap.qq.com";   p["imapPort"] = 993; p["imapSsl"] = true;
        p["pop3Host"] = "pop.qq.com";         p["pop3Port"] = 995; p["pop3Ssl"] = true;
    } else {
        // 未知域名不预设，留空让用户手动填
    }
    return p;
}
