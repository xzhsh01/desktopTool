#pragma once

#include <QObject>
#include <QVariantMap>
#include <QString>

/**
 * Settings: 应用设置持久化管理
 * 对应原 electron/main.ts 中的 settings.json 读写逻辑
 */
class Settings : public QObject {
    Q_OBJECT

public:
    static Settings& instance();

    // 加载设置文件
    void load();
    // 保存设置到文件
    void save();
    // 获取设置值
    QVariant get(const QString& key, const QVariant& defaultValue = QVariant()) const;
    // 设置值并保存
    void set(const QString& key, const QVariant& value);
    // 批量更新
    void update(const QVariantMap& changes);
    // 获取全部设置
    QVariantMap all() const;

    // 便捷 getter
    bool minimizeToTray() const;
    QString theme() const;
    QString language() const;
    int connectTimeout() const;
    bool autoCheckUpdate() const;
    QString updateUrl() const;
    QString downloadDir() const;
    int maxFileSize() const;
    QString logLevel() const;
    int logRetentionDays() const;

    // 邮件：已读回执全局策略
    //   "always" — 收到回执请求自动发回执（不弹窗）
    //   "ask"    — 弹窗询问（默认）
    //   "never"  — 永远不发送（仅记录日志）
    QString readReceiptPolicy() const;
    void    setReadReceiptPolicy(const QString& v);

    // 邮件：加密默认模式 (none/auto/smime/password)
    QString mailDefaultEncryptMode() const;
    void    setMailDefaultEncryptMode(const QString& v);

    // 邮件：默认口令（口令加密时自动用此口令，可空）
    QString mailDefaultEncryptPassword() const;
    void    setMailDefaultEncryptPassword(const QString& v);

signals:
    void changed(const QVariantMap& settings);

private:
    Settings(QObject* parent = nullptr);
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    QVariantMap m_settings;
    QString m_filePath;
    QVariantMap defaults() const;
};
