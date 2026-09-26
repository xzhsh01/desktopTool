#include "core/Labels.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QHash>
#include <QStringList>
#include <QDebug>

// 递归把 QVariantMap 展平为 "a.b.c" -> value 的扁平 hash
static void flattenInto(const QVariantMap& m, const QString& prefix, QHash<QString, QString>& out) {
    for (auto it = m.begin(); it != m.end(); ++it) {
        const QString key = prefix.isEmpty() ? it.key() : (prefix + "." + it.key());
        const QVariant v = it.value();
        if (v.typeId() == QMetaType::QVariantMap) {
            flattenInto(v.toMap(), key, out);
        } else {
            // 只接受字符串；数字/布尔统一转字符串便于直接 setText
            out.insert(key, v.toString());
        }
    }
}

Labels& Labels::instance() {
    static Labels s;
    return s;
}

Labels::Labels() {
    m_userPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                 + "/labels.json";
    loadDefault();
    loadUserOverlay();
}

void Labels::loadDefault() {
    // Qt 嵌入资源路径
    QFile f(":/labels/default.json");
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[Labels] embedded default labels.json not found";
        return;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[Labels] default labels.json parse error:" << err.errorString();
        return;
    }
    QHash<QString, QString> flat;
    flattenInto(doc.object().toVariantMap(), QString(), flat);

    // 把 default 全部并入（只填补空位；后续 user overlay 会覆盖）
    if (m_map.isEmpty()) {
        // 首次加载：直接拷
        for (auto it = flat.begin(); it != flat.end(); ++it) {
            m_map[it.key()] = it.value();
        }
    } else {
        // reload 场景：补默认缺失项
        for (auto it = flat.begin(); it != flat.end(); ++it) {
            if (!m_map.contains(it.key())) m_map[it.key()] = it.value();
        }
    }
}

void Labels::loadUserOverlay() {
    QFileInfo fi(m_userPath);
    if (!fi.exists()) return;
    QFile f(m_userPath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[Labels] cannot open user labels.json:" << m_userPath;
        return;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[Labels] user labels.json parse error:" << err.errorString();
        return;
    }
    QHash<QString, QString> flat;
    flattenInto(doc.object().toVariantMap(), QString(), flat);
    int n = 0;
    for (auto it = flat.begin(); it != flat.end(); ++it) {
        m_map[it.key()] = it.value();
        ++n;
    }
    qInfo() << "[Labels] applied" << n << "user overrides from" << m_userPath;
}

void Labels::reload() {
    m_map.clear();
    loadDefault();
    loadUserOverlay();
}

QString Labels::get(const QString& path, const QString& defaultText) const {
    auto it = m_map.constFind(path);
    if (it != m_map.constEnd()) return it.value().toString();
    // 缺失：回退 defaultText 或 key 本身（让 UI 至少能看出是哪个 key 没翻译）
    return defaultText.isEmpty() ? path : defaultText;
}

QString Labels::userFilePath() const { return m_userPath; }

int Labels::size() const { return m_map.size(); }