#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QList>
#include <QReadWriteLock>

/**
 * Logger: 日志管理
 * 对应原 electron-log 的功能，支持控制台 + 文件 + UI 查询
 */
class Logger : public QObject {
    Q_OBJECT

public:
    enum Level { Debug, Info, Warn, Error, Success };
    Q_ENUM(Level)

    struct Entry {
        int id;
        Level level;
        QString message;
        QString source;
        QDateTime timestamp;
    };

    static Logger& instance();

    void info(const QString& msg, const QString& source = "app");
    void warn(const QString& msg, const QString& source = "app");
    void error(const QString& msg, const QString& source = "app");
    void success(const QString& msg, const QString& source = "app");
    void debug(const QString& msg, const QString& source = "app");

    QList<Entry> logs() const;
    QList<Entry> filtered(Level level, const QString& keyword) const;
    void clear();
    QString logFilePath() const;

signals:
    void logAdded(const Entry& entry);
    void logsCleared();

private:
    Logger(QObject* parent = nullptr);
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void write(const Entry& entry);
    void flushToFile(const Entry& entry);

    mutable QReadWriteLock m_lock;
    QList<Entry> m_logs;
    int m_nextId = 1;
    QString m_logDir;
    QString m_logFile;
};

Q_DECLARE_METATYPE(Logger::Entry)
