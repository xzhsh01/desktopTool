#include "core/Settings.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QJsonArray>

Settings& Settings::instance() {
    static Settings s;
    return s;
}

Settings::Settings(QObject* parent) : QObject(parent) {
    // 设置文件路径: %APPDATA%/KFrame/bambooRat/settings.json
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    m_filePath = dir + "/settings.json";
    load();
}

QVariantMap Settings::defaults() const {
    return {
        {"minimizeToTray", true},
        {"theme", "dark"},
        {"language", "zh-CN"},
        {"connectTimeout", 30},
        {"autoCheckUpdate", true},
        {"updateUrl", "https://example.com/updates/"},
        {"downloadDir", ""},
        {"maxFileSize", 500},
        {"logLevel", "info"},
        {"logRetentionDays", 30},
        // 邮件：已读回执策略（ask 默认；always 自动发送；never 永不发送）
        {"readReceiptPolicy", "ask"},
        // 邮件加密默认模式
        {"mailDefaultEncryptMode", "none"},
        {"mailDefaultEncryptPassword", ""},
        // 微信：手动投放语音目录（默认 AppLocalData/voice_drop）
        {"wechat/voiceDropDir",
         QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
             + "/voice_drop"}
    };
}

void Settings::load() {
    QFile f(m_filePath);
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            m_settings = doc.object().toVariantMap();
        }
    }
    // Merge with defaults
    auto def = defaults();
    for (auto it = def.begin(); it != def.end(); ++it) {
        if (!m_settings.contains(it.key())) {
            m_settings[it.key()] = it.value();
        }
    }
}

void Settings::save() {
    QFile f(m_filePath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonDocument doc(QJsonObject::fromVariantMap(m_settings));
        f.write(doc.toJson(QJsonDocument::Indented));
    }
}

QVariant Settings::get(const QString& key, const QVariant& defaultValue) const {
    return m_settings.value(key, defaultValue);
}

void Settings::set(const QString& key, const QVariant& value) {
    m_settings[key] = value;
    save();
    emit changed(m_settings);
}

void Settings::update(const QVariantMap& changes) {
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        m_settings[it.key()] = it.value();
    }
    save();
    emit changed(m_settings);
}

QVariantMap Settings::all() const {
    return m_settings;
}

bool Settings::minimizeToTray() const { return get("minimizeToTray", true).toBool(); }
QString Settings::theme() const { return get("theme", "dark").toString(); }
QString Settings::language() const { return get("language", "zh-CN").toString(); }
int Settings::connectTimeout() const { return get("connectTimeout", 30).toInt(); }
bool Settings::autoCheckUpdate() const { return get("autoCheckUpdate", true).toBool(); }
QString Settings::updateUrl() const { return get("updateUrl").toString(); }
QString Settings::downloadDir() const { return get("downloadDir").toString(); }
int Settings::maxFileSize() const { return get("maxFileSize", 500).toInt(); }
QString Settings::logLevel() const { return get("logLevel", "info").toString(); }
int Settings::logRetentionDays() const { return get("logRetentionDays", 30).toInt(); }

QString Settings::readReceiptPolicy() const {
    QString v = get("readReceiptPolicy", "ask").toString().toLower();
    return (v == "always" || v == "ask" || v == "never") ? v : "ask";
}
void Settings::setReadReceiptPolicy(const QString& v) {
    set("readReceiptPolicy", v.toLower());
}

QString Settings::mailDefaultEncryptMode() const {
    QString v = get("mailDefaultEncryptMode", "none").toString().toLower();
    return (v == "auto" || v == "smime" || v == "password") ? v : "none";
}
void Settings::setMailDefaultEncryptMode(const QString& v) {
    set("mailDefaultEncryptMode", v.toLower());
}

QString Settings::mailDefaultEncryptPassword() const {
    return get("mailDefaultEncryptPassword").toString();
}
void Settings::setMailDefaultEncryptPassword(const QString& v) {
    // 警告：明文口令。生产应使用 core/Crypto::encrypt (DPAPI) 二次保护。
    set("mailDefaultEncryptPassword", v);
}
