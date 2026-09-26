#include "ssh/SFTPClient.h"
#include "core/Logger.h"

#include <QTcpSocket>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <fcntl.h>     // O_RDONLY / O_WRONLY / O_CREAT / O_TRUNC

// ── SFTPClient（主线程侧） ───────────────────────────────────────────────────

SFTPClient::SFTPClient(QObject* parent) : QObject(parent) {
    qRegisterMetaType<SFTPClient::FileEntry>("SFTPClient::FileEntry");
    qRegisterMetaType<QList<SFTPClient::FileEntry>>("QList<SFTPClient::FileEntry>");
}

SFTPClient::~SFTPClient() {
    disconnect();
}

void SFTPClient::connectTo(const ConnectParams& params) {
    if (m_worker && m_worker->isRunning()) {
        emit connectionError("已有 SFTP 连接，请先断开");
        return;
    }
    if (m_worker) {
        m_worker->deleteLater();
        m_worker = nullptr;
    }

    m_worker = new SFTPWorker(this);
    connect(m_worker, &SFTPWorker::connected, this, [this]() {
        setConnected(true);
        emit connected();
    });
    connect(m_worker, &SFTPWorker::disconnected, this, [this]() {
        setConnected(false);
        emit disconnected();
    });
    connect(m_worker, &SFTPWorker::connectionError, this, &SFTPClient::connectionError);
    connect(m_worker, &SFTPWorker::directoryListed, this, &SFTPClient::directoryListed);
    connect(m_worker, &SFTPWorker::downloadProgress, this, &SFTPClient::downloadProgress);
    connect(m_worker, &SFTPWorker::uploadProgress, this, &SFTPClient::uploadProgress);
    connect(m_worker, &SFTPWorker::transferFinished, this, &SFTPClient::transferFinished);
    connect(m_worker, &SFTPWorker::operationFinished, this, &SFTPClient::operationFinished);
    connect(m_worker, &SFTPWorker::statusMessage, this, &SFTPClient::statusMessage);
    connect(m_worker, &SFTPWorker::finished, this, [this]() {
        setConnected(false);
        m_worker->deleteLater();
        m_worker = nullptr;
    });

    Command cmd;
    cmd.type = Command::Connect;
    cmd.params = params;
    m_worker->queueCommand(cmd);
    m_worker->start();
}

void SFTPClient::disconnect() {
    if (m_worker) {
        Command cmd;
        cmd.type = Command::Disconnect;
        m_worker->queueCommand(cmd);
        m_worker->requestStop();
    }
}

void SFTPClient::cancelTransfer() {
    if (m_worker) m_worker->requestCancelTransfer();
}

void SFTPClient::listDirectory(const QString& remotePath) {
    if (!m_worker) { emit operationFinished("list", false, "未连接"); return; }
    Command cmd;
    cmd.type = Command::ListDir;
    cmd.path = remotePath;
    m_worker->queueCommand(cmd);
}

void SFTPClient::download(const QString& remotePath, const QString& localPath) {
    if (!m_worker) { emit transferFinished(localPath, false, "未连接"); return; }
    Command cmd;
    cmd.type = Command::Download;
    cmd.remotePath = remotePath;
    cmd.localPath = localPath;
    m_worker->queueCommand(cmd);
}

void SFTPClient::upload(const QString& localPath, const QString& remotePath) {
    if (!m_worker) { emit transferFinished(remotePath, false, "未连接"); return; }
    Command cmd;
    cmd.type = Command::Upload;
    cmd.localPath = localPath;
    cmd.remotePath = remotePath;
    m_worker->queueCommand(cmd);
}

void SFTPClient::removeFile(const QString& remotePath) {
    if (!m_worker) { emit operationFinished("delete", false, "未连接"); return; }
    Command cmd;
    cmd.type = Command::Delete;
    cmd.path = remotePath;
    m_worker->queueCommand(cmd);
}

void SFTPClient::rename(const QString& oldPath, const QString& newPath) {
    if (!m_worker) { emit operationFinished("rename", false, "未连接"); return; }
    Command cmd;
    cmd.type = Command::Rename;
    cmd.path = oldPath;
    cmd.newPath = newPath;
    m_worker->queueCommand(cmd);
}

void SFTPClient::makeDirectory(const QString& remotePath) {
    if (!m_worker) { emit operationFinished("mkdir", false, "未连接"); return; }
    Command cmd;
    cmd.type = Command::Mkdir;
    cmd.path = remotePath;
    m_worker->queueCommand(cmd);
}

void SFTPClient::removeDirectory(const QString& remotePath) {
    if (!m_worker) { emit operationFinished("rmdir", false, "未连接"); return; }
    Command cmd;
    cmd.type = Command::Rmdir;
    cmd.path = remotePath;
    m_worker->queueCommand(cmd);
}

// ── SFTPWorker（工作线程侧） ─────────────────────────────────────────────────

SFTPWorker::SFTPWorker(QObject* parent) : QThread(parent) {}

SFTPWorker::~SFTPWorker() {
    requestStop();
    wait(3000);
}

void SFTPWorker::queueCommand(const SFTPClient::Command& cmd) {
    QMutexLocker locker(&m_mutex);
    m_queue.enqueue(cmd);
}

void SFTPWorker::requestStop() {
    m_running = false;
}

void SFTPWorker::run() {
    m_running = true;

    // 等待 Connect 命令
    SFTPClient::ConnectParams params;
    while (m_running) {
        {
            QMutexLocker locker(&m_mutex);
            if (!m_queue.isEmpty() && m_queue.first().type == SFTPClient::Command::Connect) {
                params = m_queue.dequeue().params;
                break;
            }
        }
        msleep(20);
    }
    if (!m_running) {
        emit disconnected();
        return;
    }

    if (!doConnect(params)) {
        doDisconnect();
        emit disconnected();
        return;
    }
    emit connected();

    // 主循环：阻塞等待命令并处理
    while (m_running) {
        SFTPClient::Command cmd;
        bool got = false;
        {
            QMutexLocker locker(&m_mutex);
            if (!m_queue.isEmpty()) {
                cmd = m_queue.dequeue();
                got = true;
            }
        }
        if (!got) {
            msleep(50);
            continue;
        }
        switch (cmd.type) {
            case SFTPClient::Command::Disconnect:
                m_running = false;
                break;
            case SFTPClient::Command::ListDir:
                doList(cmd.path);
                break;
            case SFTPClient::Command::Download:
                doDownload(cmd.remotePath, cmd.localPath);
                break;
            case SFTPClient::Command::Upload:
                doUpload(cmd.localPath, cmd.remotePath);
                break;
            case SFTPClient::Command::Delete:
                doDelete(cmd.path);
                break;
            case SFTPClient::Command::Rename:
                doRename(cmd.path, cmd.newPath);
                break;
            case SFTPClient::Command::Mkdir:
                doMkdir(cmd.path);
                break;
            case SFTPClient::Command::Rmdir:
                doRmdir(cmd.path);
                break;
            case SFTPClient::Command::Connect:
            case SFTPClient::Command::Stat:
                break;
        }
    }

    doDisconnect();
    emit disconnected();
}

bool SFTPWorker::doConnect(const SFTPClient::ConnectParams& params) {
    m_session = ssh_new();
    if (!m_session) {
        emitError("无法创建 SSH 会话");
        return false;
    }

    int verbosity = SSH_LOG_NOLOG;
    int port = params.port > 0 ? params.port : 22;
    int timeout = params.timeoutSec > 0 ? params.timeoutSec : 30;

    ssh_options_set(m_session, SSH_OPTIONS_HOST, params.host.toUtf8().constData());
    ssh_options_set(m_session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(m_session, SSH_OPTIONS_USER, params.username.toUtf8().constData());
    ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(m_session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    ssh_options_set(m_session, SSH_OPTIONS_STRICTHOSTKEYCHECK, 0);

    // TCP 连接（用 QTcpSocket 注入 fd）
    m_socket = new QTcpSocket;
    m_socket->setReadBufferSize(0);
    m_socket->connectToHost(params.host, port);
    if (!m_socket->waitForConnected(timeout * 1000)) {
        emitError(QString("TCP 连接失败: %1").arg(m_socket->errorString()));
        return false;
    }
    socket_t fd = m_socket->socketDescriptor();
    ssh_options_set(m_session, SSH_OPTIONS_FD, &fd);

    int rc = ssh_connect(m_session);
    if (rc != SSH_OK) {
        emitError(QString("SSH 握手失败: %1").arg(ssh_get_error(m_session)));
        return false;
    }

    // 认证
    if (!params.privateKeyPath.isEmpty()) {
        ssh_key key = nullptr;
        QFileInfo ki(params.privateKeyPath);
        rc = ssh_pki_import_privkey_file(
            ki.absoluteFilePath().toLocal8Bit().constData(), nullptr, nullptr, nullptr, &key);
        if (rc != SSH_OK) {
            emitError("加载私钥失败");
            return false;
        }
        rc = ssh_userauth_publickey(m_session,
            params.username.toUtf8().constData(), key);
        ssh_key_free(key);
        if (rc != SSH_AUTH_SUCCESS) {
            emitError(QString("公钥认证失败: %1").arg(ssh_get_error(m_session)));
            return false;
        }
    } else if (!params.password.isEmpty()) {
        rc = ssh_userauth_password(m_session,
            params.username.toUtf8().constData(),
            params.password.toUtf8().constData());
        if (rc != SSH_AUTH_SUCCESS) {
            emitError(QString("密码认证失败: %1").arg(ssh_get_error(m_session)));
            return false;
        }
    } else {
        emitError("未提供密码或私钥");
        return false;
    }

    // 打开 SFTP 子系统
    m_sftp = sftp_new(m_session);
    if (!m_sftp) {
        emitError(QString("无法创建 SFTP 会话: %1").arg(ssh_get_error(m_session)));
        return false;
    }
    rc = sftp_init(m_sftp);
    if (rc != SSH_OK) {
        emitError(QString("SFTP 初始化失败: %1").arg(sftp_get_error(m_sftp)));
        return false;
    }

    return true;
}

void SFTPWorker::doDisconnect() {
    if (m_sftp) {
        sftp_free(m_sftp);
        m_sftp = nullptr;
    }
    if (m_session) {
        ssh_disconnect(m_session);
        ssh_free(m_session);
        m_session = nullptr;
    }
    if (m_socket) {
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void SFTPWorker::doList(const QString& path) {
    if (!m_sftp) {
        emit operationFinished("list", false, "未连接");
        return;
    }

    sftp_dir dir = sftp_opendir(m_sftp, path.toUtf8().constData());
    if (!dir) {
        emitError(QString("打开目录失败: %1").arg(path));
        emit operationFinished("list", false, path);
        return;
    }

    QList<SFTPClient::FileEntry> entries;
    while (true) {
        sftp_attributes attr = sftp_readdir(m_sftp, dir);
        if (!attr) {
            if (sftp_dir_eof(dir)) break;
            // 暂时性错误，跳过继续
            if (attr == nullptr) break;
        }
        // skip "." and ".."
        QString name = QString::fromUtf8(attr->name);
        if (name == "." || name == "..") {
            sftp_attributes_free(attr);
            continue;
        }

        SFTPClient::FileEntry e;
        e.name = name;
        e.longName = QString::fromUtf8(attr->longname ? attr->longname : "");
        e.isDir = (attr->type == SSH_FILEXFER_TYPE_DIRECTORY);
        e.isLink = (attr->type == SSH_FILEXFER_TYPE_SYMLINK);
        e.size = static_cast<qint64>(attr->size);
        e.permissions = attr->permissions;
        if (attr->mtime != 0) {
            e.mtime = QDateTime::fromSecsSinceEpoch(
                static_cast<qint64>(attr->mtime));
        }
        entries.append(e);
        sftp_attributes_free(attr);
    }
    sftp_closedir(dir);

    emit directoryListed(path, entries);
    emit operationFinished("list", true, path);
}

void SFTPWorker::doDownload(const QString& remote, const QString& local) {
    if (!m_sftp) {
        emit transferFinished(local, false, "未连接");
        return;
    }

    QFile localFile(local);
    if (!localFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit transferFinished(local, false, QString("打开本地文件失败: %1").arg(localFile.errorString()));
        return;
    }

    sftp_file file = sftp_open(m_sftp, remote.toUtf8().constData(),
                                O_RDONLY, 0);
    if (!file) {
        localFile.close();
        emit transferFinished(local, false, QString("打开远端文件失败: %1")
            .arg(sftp_get_error(m_sftp)));
        return;
    }

    char buf[64*1024];
    bool ok = true;
    bool cancelled = false;
    QString error;
    qint64 total = 0;
    qint64 done = 0;
    qint64 lastEmitted = -1;
    m_cancelRequested.store(false);

    // 先取远端文件大小用于进度显示
    sftp_attributes attr = sftp_stat(m_sftp, remote.toUtf8().constData());
    if (attr) {
        total = static_cast<qint64>(attr->size);
        sftp_attributes_free(attr);
    }
    emit downloadProgress(remote, 0, total);

    while (true) {
        if (m_cancelRequested.load()) {
            cancelled = true;
            ok = false;
            error = "已取消";
            break;
        }
        ssize_t n = sftp_read(file, buf, sizeof(buf));
        if (n == 0) break;       // EOF
        if (n < 0) {
            ok = false;
            error = QString("读取失败: %1").arg(sftp_get_error(m_sftp));
            break;
        }
        if (localFile.write(buf, n) != n) {
            ok = false;
            error = QString("写入本地文件失败: %1").arg(localFile.errorString());
            break;
        }
        done += n;
        if (done - lastEmitted >= 256 * 1024 || (total > 0 && done >= total)) {
            lastEmitted = done;
            emit downloadProgress(remote, done, total);
        }
    }
    sftp_close(file);
    localFile.close();

    if (ok) {
        emit downloadProgress(remote, done, total);
        emit transferFinished(local, true, QString());
        emit statusMessage(QString("下载完成: %1").arg(remote));
    } else {
        emit transferFinished(local, false, error);
    }
}

void SFTPWorker::doUpload(const QString& local, const QString& remote) {
    if (!m_sftp) {
        emit transferFinished(remote, false, "未连接");
        return;
    }

    QFile localFile(local);
    if (!localFile.open(QIODevice::ReadOnly)) {
        emit transferFinished(remote, false, QString("打开本地文件失败: %1").arg(localFile.errorString()));
        return;
    }

    sftp_file file = sftp_open(m_sftp, remote.toUtf8().constData(),
                                O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!file) {
        localFile.close();
        emit transferFinished(remote, false, QString("打开远端文件失败: %1")
            .arg(sftp_get_error(m_sftp)));
        return;
    }

    char buf[64*1024];
    bool ok = true;
    bool cancelled = false;
    QString error;
    const qint64 total = localFile.size();
    qint64 done = 0;
    qint64 lastEmitted = -1;
    m_cancelRequested.store(false);
    emit uploadProgress(remote, 0, total);

    while (true) {
        if (m_cancelRequested.load()) {
            cancelled = true;
            ok = false;
            error = "已取消";
            break;
        }
        qint64 n = localFile.read(buf, sizeof(buf));
        if (n == 0) break;       // EOF
        if (n < 0) {
            ok = false;
            error = QString("读取本地文件失败: %1").arg(localFile.errorString());
            break;
        }
        // sftp_write 允许短写（部分写入，sftp_get_error 返回 0），
        // 必须循环补写剩余字节；仅返回值 <= 0 才是真正的错误
        const size_t toWrite = static_cast<size_t>(n);
        size_t off = 0;
        while (off < toWrite) {
            if (m_cancelRequested.load()) {
                cancelled = true;
                break;
            }
            ssize_t w = sftp_write(file, buf + off, toWrite - off);
            if (w <= 0) {
                ok = false;
                error = QString("写入远端失败: %1").arg(sftp_get_error(m_sftp));
                break;
            }
            off += static_cast<size_t>(w);
        }
        if (!ok || cancelled) break;
        done += n;
        // 进度节流：每 256KB 或完成时上报一次
        if (done - lastEmitted >= 256 * 1024 || done >= total) {
            lastEmitted = done;
            emit uploadProgress(remote, done, total);
        }
    }
    sftp_close(file);
    localFile.close();

    if (ok) {
        emit uploadProgress(remote, total, total);
        emit transferFinished(remote, true, QString());
        emit statusMessage(QString("上传完成: %1").arg(remote));
    } else {
        emit transferFinished(remote, false, error);
    }
}

void SFTPWorker::doDelete(const QString& path) {
    if (!m_sftp) { emit operationFinished("delete", false, "未连接"); return; }
    int rc = sftp_unlink(m_sftp, path.toUtf8().constData());
    if (rc != SSH_OK) {
        emitError(QString("删除失败: %1").arg(path));
        emit operationFinished("delete", false, path);
    } else {
        emit operationFinished("delete", true, path);
    }
}

void SFTPWorker::doRename(const QString& oldPath, const QString& newPath) {
    if (!m_sftp) { emit operationFinished("rename", false, "未连接"); return; }
    int rc = sftp_rename(m_sftp,
                          oldPath.toUtf8().constData(),
                          newPath.toUtf8().constData());
    if (rc != SSH_OK) {
        emitError(QString("重命名失败: %1 → %2").arg(oldPath, newPath));
        emit operationFinished("rename", false, oldPath);
    } else {
        emit operationFinished("rename", true, newPath);
    }
}

void SFTPWorker::doMkdir(const QString& path) {
    if (!m_sftp) { emit operationFinished("mkdir", false, "未连接"); return; }
    int rc = sftp_mkdir(m_sftp, path.toUtf8().constData(), 0755);
    if (rc != SSH_OK) {
        emitError(QString("创建目录失败: %1").arg(path));
        emit operationFinished("mkdir", false, path);
    } else {
        emit operationFinished("mkdir", true, path);
    }
}

void SFTPWorker::doRmdir(const QString& path) {
    if (!m_sftp) { emit operationFinished("rmdir", false, "未连接"); return; }
    int rc = sftp_rmdir(m_sftp, path.toUtf8().constData());
    if (rc != SSH_OK) {
        emitError(QString("删除目录失败: %1").arg(path));
        emit operationFinished("rmdir", false, path);
    } else {
        emit operationFinished("rmdir", true, path);
    }
}

void SFTPWorker::emitError(const QString& context) {
    QString detail;
    if (m_session) {
        detail = QString::fromUtf8(ssh_get_error(m_session));
    } else if (m_sftp) {
        // sftp_get_error 返回错误码（如 SSH_FX_*），而非字符串
        detail = QString("SFTP 错误码 %1").arg(sftp_get_error(m_sftp));
    } else {
        detail = "未知错误";
    }
    QString full = detail.isEmpty()
        ? context
        : QString("%1: %2").arg(context, detail);
    emit connectionError(full);
    Logger::instance().error(full, "sftp");
}