#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QQueue>
#include <QDateTime>
#include <atomic>

// libssh C API（对调用方隐藏）
struct ssh_session_struct;
typedef struct ssh_session_struct* ssh_session;
struct sftp_session_struct;
typedef struct sftp_session_struct* sftp_session;

/**
 * SFTPClient: SFTP 文件传输客户端（libssh 封装）
 *
 * 与 SSHClient 共用同一套 SSH 协议栈——理论上可合并，
 * 但为了与终端会话独立（不同连接参数、不同生命周期），
 * 单独建立 SSH session 简化状态管理。
 *
 * 所有阻塞的 libssh SFTP 调用在工作线程中执行。
 */
class SFTPClient : public QObject {
    Q_OBJECT

public:
    // 远端文件条目
    struct FileEntry {
        QString name;
        QString longName;
        bool isDir = false;
        bool isLink = false;
        qint64 size = 0;
        QDateTime mtime;
        quint32 permissions = 0;
    };

    struct ConnectParams {
        QString host;
        int port = 22;
        QString username;
        QString password;
        QString privateKeyPath;
        int timeoutSec = 30;
    };

    // 内部命令
    struct Command {
        enum Type {
            Connect, Disconnect,
            ListDir, Download, Upload,
            Delete, Rename, Mkdir, Rmdir, Stat
        } type;
        ConnectParams params;
        QString path;                  // ListDir/Delete/Mkdir/Rmdir/Stat
        QString remotePath;            // Download/Upload
        QString localPath;             // Download/Upload
        QString newPath;               // Rename
    };

    explicit SFTPClient(QObject* parent = nullptr);
    ~SFTPClient();

    // 公共 API（线程安全：命令入队）
    void connectTo(const ConnectParams& params);
    void disconnect();
    void listDirectory(const QString& remotePath);
    void download(const QString& remotePath, const QString& localPath);
    void upload(const QString& localPath, const QString& remotePath);
    void removeFile(const QString& remotePath);
    void rename(const QString& oldPath, const QString& newPath);
    void makeDirectory(const QString& remotePath);
    void removeDirectory(const QString& remotePath);
    void cancelTransfer();   // 请求取消当前正在进行的上传/下载

    bool isConnected() const { return m_connected; }

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& message);
    void directoryListed(const QString& path, const QList<FileEntry>& entries);
    void downloadProgress(const QString& path, qint64 bytesDone, qint64 bytesTotal);
    void uploadProgress(const QString& path, qint64 bytesDone, qint64 bytesTotal);
    void transferFinished(const QString& path, bool success, const QString& error);
    void operationFinished(const QString& op, bool success, const QString& error);
    void statusMessage(const QString& message);

private:
    void setConnected(bool v) { m_connected = v; }

    class SFTPWorker* m_worker = nullptr;
    bool m_connected = false;
};

Q_DECLARE_METATYPE(SFTPClient::FileEntry)
Q_DECLARE_METATYPE(QList<SFTPClient::FileEntry>)

/**
 * SFTPWorker: SFTP 工作线程
 */
class SFTPWorker : public QThread {
    Q_OBJECT

public:
    explicit SFTPWorker(QObject* parent = nullptr);
    ~SFTPWorker();

    void queueCommand(const SFTPClient::Command& cmd);
    void requestStop();
    // 请求取消当前传输（跨线程原子标志，传输循环中检查）
    void requestCancelTransfer() { m_cancelRequested.store(true); }

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& message);
    void directoryListed(const QString& path, const QList<SFTPClient::FileEntry>& entries);
    void downloadProgress(const QString& path, qint64 bytesDone, qint64 bytesTotal);
    void uploadProgress(const QString& path, qint64 bytesDone, qint64 bytesTotal);
    void transferFinished(const QString& path, bool success, const QString& error);
    void operationFinished(const QString& op, bool success, const QString& error);
    void statusMessage(const QString& message);

protected:
    void run() override;

private:
    bool doConnect(const SFTPClient::ConnectParams& params);
    void doDisconnect();
    void doList(const QString& path);
    void doDownload(const QString& remote, const QString& local);
    void doUpload(const QString& local, const QString& remote);
    void doDelete(const QString& path);
    void doRename(const QString& oldPath, const QString& newPath);
    void doMkdir(const QString& path);
    void doRmdir(const QString& path);
    void emitError(const QString& context);

    // libssh 句柄（仅工作线程访问）
    ssh_session m_session = nullptr;
    sftp_session m_sftp = nullptr;
    class QTcpSocket* m_socket = nullptr;
    bool m_running = false;
    std::atomic<bool> m_cancelRequested{false};   // 传输取消标志

    QMutex m_mutex;
    QQueue<SFTPClient::Command> m_queue;
};