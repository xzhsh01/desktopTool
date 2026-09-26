#pragma once
#include <QString>
#include <QByteArray>

/**
 * 嵌入式 SQLite 配置存储（单例）
 *
 * 所有配置信息统一写入 %APPDATA%/KFrame/bambooRat/config.db：
 *   表 config(domain TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at TEXT)
 *   - domain: settings / connections / mail_accounts
 *   - value : JSON 序列化文本（沿用原有 JSON 结构，仅更换存储介质）
 *
 * 实现说明：
 *   - 使用 SQLite C API（sqlite3.h）。SQLite 是纯 C 接口，
 *     MinGW 下无 STL 跨边界的 ABI 风险，直接以源码方式编译进项目。
 *   - 头文件以 void* 隐藏句柄，避免 sqlite3.h 泄漏到其他编译单元。
 *   - 旧 JSON 文件在首次启动时一次性迁移入库（改名 *.migrated.bak 保留备份）。
 */
class ConfigStore {
public:
    static ConfigStore& instance();
    ~ConfigStore();

    bool isOpen() const { return m_db != nullptr; }

    /// 读取域的 JSON 文本；无数据/未打开返回空 QByteArray
    QByteArray readDomain(const QString& domain);

    /// 写入域（INSERT OR REPLACE），返回是否成功
    bool writeDomain(const QString& domain, const QByteArray& json);

    /// 一次性迁移：DB 无该域且 JSON 文件存在 → 导入并备份旧文件（*.migrated.bak）
    bool migrateFromFile(const QString& domain, const QString& jsonPath);

private:
    ConfigStore();
    bool ensureOpen();

    void* m_db = nullptr;    // sqlite3*
};
