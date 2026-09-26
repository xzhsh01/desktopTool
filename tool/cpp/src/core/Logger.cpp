#include "core/Logger.h"
#include "core/Settings.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QFileInfo>

Logger& Logger::instance() {
    static Logger l;
    return l;
}

Logger::Logger(QObject* parent) : QObject(parent) {
    m_logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
    QDir().mkpath(m_logDir);
    m_logFile = m_logDir + "/app.log";
}

void Logger::info(const QString& msg, const QString& source) {
    write({m_nextId++, Info, msg, source, QDateTime::currentDateTime()});
}
void Logger::warn(const QString& msg, const QString& source) {
    write({m_nextId++, Warn, msg, source, QDateTime::currentDateTime()});
}
void Logger::error(const QString& msg, const QString& source) {
    write({m_nextId++, Error, msg, source, QDateTime::currentDateTime()});
}
void Logger::success(const QString& msg, const QString& source) {
    write({m_nextId++, Success, msg, source, QDateTime::currentDateTime()});
}
void Logger::debug(const QString& msg, const QString& source) {
    write({m_nextId++, Debug, msg, source, QDateTime::currentDateTime()});
}

void Logger::write(const Entry& entry) {
    {
        QWriteLocker locker(&m_lock);
        m_logs.append(entry);
        if (m_logs.size() > 1000) {
            m_logs.removeFirst();
        }
    }
    flushToFile(entry);
    emit logAdded(entry);
}

void Logger::flushToFile(const Entry& entry) {
    static const char* levelStr[] = {"DEBUG", "INFO", "WARN", "ERROR", "SUCCESS"};
    int levelIdx = static_cast<int>(entry.level);
    if (levelIdx < 0) levelIdx = 0;

    // Check log level filter
    QString configuredLevel = Settings::instance().logLevel();
    // Simple level filtering: debug < info < warn < error
    // If configured level is "warn", only write warn, error, success
    // (For simplicity, write all to file for now)

    QFile f(m_logFile);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts << QString("[%1] [%2] [%3] %4\n")
              .arg(entry.timestamp.toString(Qt::ISODate))
              .arg(levelStr[levelIdx])
              .arg(entry.source)
              .arg(entry.message);
    }
}

QList<Logger::Entry> Logger::logs() const {
    QReadLocker locker(&m_lock);
    return m_logs;
}

QList<Logger::Entry> Logger::filtered(Level level, const QString& keyword) const {
    QReadLocker locker(&m_lock);
    QList<Entry> result;
    for (const auto& e : m_logs) {
        // level == -1 表示全部级别
        if (level != static_cast<Level>(-1) && e.level != level) continue;
        if (!keyword.isEmpty()) {
            if (!e.message.contains(keyword, Qt::CaseInsensitive) &&
                !e.source.contains(keyword, Qt::CaseInsensitive))
                continue;
        }
        result.append(e);
    }
    return result;
}

void Logger::clear() {
    {
        QWriteLocker locker(&m_lock);
        m_logs.clear();
    }
    emit logsCleared();
}

QString Logger::logFilePath() const {
    return m_logFile;
}
