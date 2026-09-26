#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QList>

class QSqlDatabase;

/**
 * AttachmentStore: 文档/附件统一存储（数据层）
 *
 * 设计要点：
 *  - 元数据存 SQLite，二进制按 SHA-256 散列存到 %APPDATA%/KFrame/bambooRat/attachments/
 *  - 支持按内容去重（同一 sha256 文件只存一份，但允许多条元数据记录指向它）
 *  - source 字段区分 manual（手动上传）/ email（邮件附件，后续接入 IMAP）
 *  - 单例：通过 AttachmentStore::instance() 获取
 */
class AttachmentStore : public QObject {
    Q_OBJECT

public:
    struct Attachment {
        qint64  id = 0;
        QString sha256;
        QString originalName;      // 原始文件名（含扩展名）
        QString storedPath;        // 相对 attachments/ 的路径
        QString mimeType;
        qint64  size = 0;
        QString source;            // "manual" / "email"
        QString sourceRef;         // 邮件 ID / null
        QString accountId;         // 来源账号 ID（邮件账号等），可空
        QString tags;              // 逗号分隔标签
        QString description;
        QDateTime uploadedAt;
    };

    // 过滤选项（list() / count() 通用）
    struct Filter {
        QString source;            // 空 = 全部；"manual" / "email"
        QString keyword;           // 模糊匹配 originalName / tags / description
        QString tag;               // 精确匹配单个标签
    };

    static AttachmentStore& instance();

    // 初始化：打开 SQLite、创建表、确保 attachments/ 目录存在
    void init();

    // 导入文件（拷贝到 attachments/，按 sha256 去重；返回新增或已存在的记录 id）
    // 参数：
    //   sourceFilePath: 源文件路径
    //   source: "manual" 或 "email"
    //   sourceRef/accountId/tags/description: 元数据
    // 失败时返回 -1，errorMessage 包含原因
    qint64 importFile(const QString& sourceFilePath,
                      const QString& source,
                      const QString& sourceRef,
                      const QString& accountId,
                      const QString& tags,
                      const QString& description,
                      QString* errorMessage = nullptr);

    // 通过已有字节导入（供 IMAP 附件内存中下载时使用）
    qint64 importBytes(const QByteArray& data,
                       const QString& fileName,
                       const QString& source,
                       const QString& sourceRef,
                       const QString& accountId,
                       const QString& tags,
                       const QString& description,
                       QString* errorMessage = nullptr);

    // 查询
    QList<Attachment> list(const Filter& f = Filter()) const;
    int               count(const Filter& f = Filter()) const;
    Attachment        getById(qint64 id) const;
    qint64            totalSize() const;

    // 删除：仅元数据 + 当 sha256 不再被任何记录引用时，连同文件一起删
    bool remove(qint64 id, QString* errorMessage = nullptr);

    // 更新标签（覆盖式）
    bool updateTags(qint64 id, const QString& tags, QString* errorMessage = nullptr);

    // 更新描述
    bool updateDescription(qint64 id, const QString& description, QString* errorMessage = nullptr);

    // 导出到用户指定位置（用于"另存为"）
    bool exportTo(qint64 id, const QString& destPath, QString* errorMessage = nullptr);

    // 存储根目录（attachments/ 绝对路径）
    QString storageDir() const { return m_storageDir; }

    // 全部已用标签（去重）
    QStringList allTags() const;

signals:
    void changed();

private:
    explicit AttachmentStore(QObject* parent = nullptr);
    ~AttachmentStore() override;
    AttachmentStore(const AttachmentStore&) = delete;
    AttachmentStore& operator=(const AttachmentStore&) = delete;

    bool ensureSchema();
    // 计算 sha256 并写入 attachments/<sha2>.<ext>，返回相对存储路径
    QString storeBytes(const QByteArray& data, const QString& fileName, QString* errorMessage);
    // 加载一条元数据
    Attachment rowToAttachment(void* stmtHandle) const;

    QString m_dbPath;
    QString m_storageDir;
    QString m_connName;
    bool    m_initialized = false;
};
