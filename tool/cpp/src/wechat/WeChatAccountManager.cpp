#include "WeChatAccountManager.h"
#include "core/Crypto.h"
#include "core/Logger.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QRegularExpression>
#include <QUuid>

WeChatAccountManager& WeChatAccountManager::instance() {
    static WeChatAccountManager inst;
    return inst;
}

WeChatAccountManager::WeChatAccountManager(QObject* parent) : QObject(parent) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    m_filePath = dir + "/wechat_accounts.json";
    load();
    Logger::instance().info(
        QString("WeChatAccountManager 初始化: accounts=%1, file=%2")
            .arg(m_accounts.size()).arg(m_filePath),
        "wechat");
}

void WeChatAccountManager::load() {
    QFile f(m_filePath);
    if (!f.open(QIODevice::ReadOnly)) return;

    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isArray()) return;

    m_accounts.clear();
    for (const auto& v : doc.array()) {
        auto o = v.toObject();
        Account a;
        a.id        = o["id"].toString();
        a.name       = o["name"].toString();
        a.wxid       = o["wxid"].toString();
        a.dataDir    = o["dataDir"].toString();
        a.keyHex     = o["keyHex"].toString();   // DPAPI 密文
        a.imageKeyHex = o["imageKeyHex"].toString(); // V2 图片 AES-128-ECB key（明文 hex）
        a.version    = o["version"].toString();
        a.createdAt  = QDateTime::fromString(o["createdAt"].toString(), Qt::ISODate);
        a.updatedAt  = QDateTime::fromString(o["updatedAt"].toString(), Qt::ISODate);
        if (!a.id.isEmpty() && !a.dataDir.isEmpty())
            m_accounts.append(a);
    }
}

void WeChatAccountManager::save() {
    QJsonArray arr;
    for (const auto& a : m_accounts) {
        QJsonObject o;
        o["id"]        = a.id;
        o["name"]      = a.name;
        o["wxid"]      = a.wxid;
        o["dataDir"]   = a.dataDir;
        o["keyHex"]    = a.keyHex;
        o["imageKeyHex"] = a.imageKeyHex;
        o["version"]   = a.version;
        o["createdAt"] = a.createdAt.toString(Qt::ISODate);
        o["updatedAt"] = a.updatedAt.toString(Qt::ISODate);
        arr.append(o);
    }
    QFile f(m_filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

WeChatAccountManager::Account* WeChatAccountManager::getById(const QString& id) {
    for (auto& a : m_accounts)
        if (a.id == id) return &a;
    return nullptr;
}

WeChatAccountManager::Account* WeChatAccountManager::getByWxid(const QString& wxid) {
    for (auto& a : m_accounts)
        if (a.wxid == wxid) return &a;
    return nullptr;
}

WeChatAccountManager::Account* WeChatAccountManager::add(const QVariantMap& data) {
    const QString wxid = data.value("wxid").toString().trimmed();
    // 同 wxid 已存在：拒绝添加（避免重复配置导致解密冲突）
    if (!wxid.isEmpty() && getByWxid(wxid)) return nullptr;

    Account a;
    a.id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
    a.name      = data.value("name").toString();
    a.wxid      = wxid;
    a.dataDir   = data.value("dataDir").toString();
    a.version   = data.value("version").toString();
    a.createdAt  = QDateTime::currentDateTime();
    a.updatedAt  = a.createdAt;

    // 密钥 DPAPI 加密存储
    QString key = data.value("keyHex").toString().trimmed();
    if (!key.isEmpty()) {
        if (Crypto::instance().isAvailable())
            a.keyHex = Crypto::instance().encrypt(key);
        else
            a.keyHex = key;   // DPAPI 不可用时明文兜底
    }
    // 图片 AES key（明文 hex，32 字符 = 16 字节；非敏感）
    a.imageKeyHex = data.value("imageKeyHex").toString().trimmed();

    m_accounts.append(a);
    save();
    emit changed();
    return &m_accounts.last();
}

bool WeChatAccountManager::update(const QString& id, const QVariantMap& data) {
    auto* a = getById(id);
    if (!a) return false;

    if (data.contains("name"))    a->name    = data.value("name").toString();
    if (data.contains("wxid"))    a->wxid    = data.value("wxid").toString();
    if (data.contains("dataDir")) a->dataDir = data.value("dataDir").toString();
    if (data.contains("version")) a->version = data.value("version").toString();
    if (data.contains("keyHex")) {
        QString key = data.value("keyHex").toString().trimmed();
        if (!key.isEmpty()) {
            a->keyHex = Crypto::instance().isAvailable()
                        ? Crypto::instance().encrypt(key) : key;
        }
    }
    if (data.contains("imageKeyHex")) {
        a->imageKeyHex = data.value("imageKeyHex").toString().trimmed();
    }
    a->updatedAt = QDateTime::currentDateTime();
    save();
    emit changed();
    return true;
}

void WeChatAccountManager::remove(const QString& id) {
    for (int i = 0; i < m_accounts.size(); ++i) {
        if (m_accounts[i].id == id) {
            m_accounts.removeAt(i);
            save();
            emit changed();
            return;
        }
    }
}

QString WeChatAccountManager::keyForAccount(const Account& acc) const {
    if (acc.keyHex.isEmpty()) return {};
    // DPAPI 加密存储：解密；失败则按明文处理（兼容 DPAPI 不可用时的落盘）
    if (Crypto::instance().isAvailable()) {
        QString plain = Crypto::instance().decrypt(acc.keyHex);
        if (!plain.isEmpty()) return plain;
    }
    return acc.keyHex;
}

QString WeChatAccountManager::documentsDir() {
    // 注册表「个人」目录（可能被重定向到非系统盘）
    QSettings reg(QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\"
        "Explorer\\User Shell Folders"), QSettings::NativeFormat);
    QString personal = reg.value("Personal").toString();
    if (!personal.isEmpty()) {
        personal = QDir::fromNativeSeparators(
            personal.replace("%USERPROFILE%", qEnvironmentVariable("USERPROFILE")));
        if (QDir(personal).exists()) return personal;
    }
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

QString WeChatAccountManager::defaultWeChatFilesRoot() {
    // 微信 3.x 允许自定义「文件存储位置」，写在全局配置 ini 中
    const QString ini = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + "/../../Tencent/WeChat/All Users/config/3ebffe94.ini";
    QString cfg = QDir::cleanPath(QDir::fromNativeSeparators(ini));
    QFile f(cfg);
    if (f.open(QIODevice::ReadOnly)) {
        QString line = QString::fromUtf8(f.readLine()).trimmed();
        f.close();
        if (!line.isEmpty() && QDir(line).exists())
            return QDir::fromNativeSeparators(line);
    }
    // 默认：文档目录
    return documentsDir();
}

QList<WeChatAccountManager::DiscoveredAccount>
WeChatAccountManager::discoverLocalAccounts() const {
    QList<DiscoveredAccount> result;
    QStringList roots;
    const QString defRoot = defaultWeChatFilesRoot();
    if (!defRoot.isEmpty()) roots << defRoot;
    const QString docs = documentsDir();
    if (!docs.isEmpty() && !roots.contains(docs)) roots << docs;

    // ── 微信 4.x 自定义存储位置 ──
    // 4.x 不走 3.x 的 3ebffe94.ini，而是写在
    //   %APPDATA%/Tencent/xwechat/config/<md5>.ini
    // 文件内容为存储根路径（如 "E:\"），数据实际位于其下 xwechat_files\ 目录。
    // 遍历该目录所有 ini，把有效路径补进扫描根（默认「文档」之外的自定义位置靠它发现）。
    {
        const QString xcfgDir = QDir::cleanPath(QDir::fromNativeSeparators(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + "/../../Tencent/xwechat/config"));
        const QFileInfoList inis = QDir(xcfgDir).entryInfoList(
            QStringList() << "*.ini", QDir::Files);
        for (const QFileInfo& ini : inis) {
            QFile f(ini.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) continue;
            QString line = QDir::fromNativeSeparators(
                QString::fromUtf8(f.readAll()).trimmed());
            f.close();
            if (line.isEmpty()) continue;
            QString root = QDir::cleanPath(line);
            // 内容也可能直接指到 xwechat_files 本身，统一还原为其父目录
            if (root.endsWith(QStringLiteral("/xwechat_files")))
                root.chop(QStringLiteral("/xwechat_files").size());
            if (root.isEmpty() || roots.contains(root)) continue;
            if (QDir(root + "/xwechat_files").exists())
                roots << root;
        }
    }

    static const QRegularExpression wxidRe(
        "^wxid_[a-zA-Z0-9_-]+$");

    for (const QString& root : roots) {
        // ── 微信 3.x: <root>\WeChat Files\wxid_* ──
        QDir wf(root + "/WeChat Files");
        if (wf.exists()) {
            for (const QFileInfo& fi : wf.entryInfoList(
                     QStringList() << "wxid_*", QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString wxid = fi.fileName();
                if (wxidRe.match(wxid).hasMatch()) {
                    // 3.x 判定：Msg\MicroMsg.db 或 Msg\Multi\MSG*.db 存在
                    const bool hasMicro = QFile::exists(fi.absoluteFilePath() + "/Msg/MicroMsg.db");
                    const bool hasMsg  = QDir(fi.absoluteFilePath() + "/Msg/Multi")
                                             .entryList(QStringList() << "MSG*.db").size() > 0;
                    if (hasMicro || hasMsg) {
                        DiscoveredAccount d;
                        d.wxid = wxid;
                        d.dataDir = fi.absoluteFilePath();
                        d.version = "3.x";
                        d.nickname = QString();  // 昵称需解密数据库后才能读到
                        result.append(d);
                    }
                }
            }
        }

        // ── 微信 4.x: <root>\xwechat_files\wxid_* ──
        QDir xwf(root + "/xwechat_files");
        if (xwf.exists()) {
            for (const QFileInfo& fi : xwf.entryInfoList(
                     QStringList() << "wxid_*", QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString wxid = fi.fileName();
                if (wxidRe.match(wxid).hasMatch()) {
                    if (QFile::exists(fi.absoluteFilePath() + "/db_storage")) {
                        DiscoveredAccount d;
                        d.wxid = wxid;
                        d.dataDir = fi.absoluteFilePath();
                        d.version = "4.x";
                        result.append(d);
                    }
                }
            }
        }
    }
    return result;
}
