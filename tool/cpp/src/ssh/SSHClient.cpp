#include "ssh/SSHClient.h"
#include "core/Logger.h"

#include <QTcpSocket>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include <libssh/libssh.h>

// ── SSHClient（主线程侧） ────────────────────────────────────────────────────

SSHClient::SSHClient(QObject* parent) : QObject(parent) {
}

SSHClient::~SSHClient() {
    disconnect();
}

void SSHClient::connectTo(const ConnectParams& params) {
    if (m_worker && m_worker->isRunning()) {
        emit connectionError("已有连接正在进行，请先断开");
        return;
    }

    // 清理旧 worker
    if (m_worker) {
        m_worker->deleteLater();
        m_worker = nullptr;
    }

    m_worker = new SSHWorker(this);

    // 转发工作线程信号
    connect(m_worker, &SSHWorker::connected, this, &SSHClient::connected);
    connect(m_worker, &SSHWorker::disconnected, this, [this]() {
        setConnected(false);
        emit disconnected();
    });
    connect(m_worker, &SSHWorker::connectionError, this, &SSHClient::connectionError);
    connect(m_worker, &SSHWorker::dataReceived, this, &SSHClient::dataReceived);
    connect(m_worker, &SSHWorker::statusMessage, this, &SSHClient::statusMessage);
    connect(m_worker, &SSHWorker::finished, this, [this]() {
        setConnected(false);
        m_worker->deleteLater();
        m_worker = nullptr;
    });

    Command cmd;
    cmd.type = Command::Connect;
    cmd.params = params;
    m_worker->queueCommand(cmd);
    m_worker->start();

    setConnected(true);  // 乐观标记，连接失败时会收到 connectionError
}

void SSHClient::disconnect() {
    if (m_worker) {
        Command cmd;
        cmd.type = Command::Disconnect;
        m_worker->queueCommand(cmd);
        m_worker->requestStop();
    }
}

void SSHClient::write(const QByteArray& data) {
    if (m_worker) {
        Command cmd;
        cmd.type = Command::Write;
        cmd.data = data;
        m_worker->queueCommand(cmd);
    }
}

void SSHClient::resizeTerminal(int cols, int rows) {
    if (m_worker) {
        Command cmd;
        cmd.type = Command::Resize;
        cmd.cols = cols;
        cmd.rows = rows;
        m_worker->queueCommand(cmd);
    }
}

void SSHClient::setConnected(bool v) {
    m_connected = v;
}

// ── SSHWorker（工作线程侧） ──────────────────────────────────────────────────

SSHWorker::SSHWorker(QObject* parent) : QThread(parent) {
}

SSHWorker::~SSHWorker() {
    requestStop();
    wait(3000);
}

void SSHWorker::queueCommand(const SSHClient::Command& cmd) {
    QMutexLocker locker(&m_mutex);
    m_queue.enqueue(cmd);
}

void SSHWorker::requestStop() {
    m_running = false;
}

void SSHWorker::run() {
    m_running = true;

    // 等待 Connect 命令
    while (m_running) {
        {
            QMutexLocker locker(&m_mutex);
            if (!m_queue.isEmpty() && m_queue.first().type == SSHClient::Command::Connect) {
                m_params = m_queue.dequeue().params;
                break;
            }
        }
        msleep(20);
    }

    if (!m_running) {
        emit disconnected();
        return;
    }

    // 发起连接
    if (!doConnect(m_params)) {
        doDisconnect();
        emit disconnected();
        return;
    }

    emit connected();
    emit statusMessage(QString("已连接 %1@%2:%3")
        .arg(m_params.username, m_params.host).arg(m_params.port));

    // 主循环：每 ~10ms 检查一次 socket 可读 + 命令队列
    while (m_running) {
        int n = pumpRead();
        if (n < 0) {
            // EOF 或错误
            break;
        }
        processQueue();
        msleep(10);  // 短暂休眠避免 CPU 100%
    }

    doDisconnect();
    emit disconnected();
}

bool SSHWorker::doConnect(const SSHClient::ConnectParams& params) {
    m_session = ssh_new();
    if (!m_session) {
        emitError("无法创建 SSH 会话");
        return false;
    }

    // 设置选项
    int verbosity = SSH_LOG_NOLOG;
    int port = params.port > 0 ? params.port : 22;
    int timeout = params.timeoutSec > 0 ? params.timeoutSec : 30;

    ssh_options_set(m_session, SSH_OPTIONS_HOST, params.host.toUtf8().constData());
    ssh_options_set(m_session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(m_session, SSH_OPTIONS_USER, params.username.toUtf8().constData());
    ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(m_session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    // 跳过已知主机检查（GUI 应用场景）
    ssh_options_set(m_session, SSH_OPTIONS_STRICTHOSTKEYCHECK, 0);

    // 先建立 TCP 连接（用 QTcpSocket 拿 socket fd，避免 libssh 的 DNS 阻塞）
    m_socket = new QTcpSocket;
    m_socket->setReadBufferSize(0);
    m_socket->connectToHost(params.host, port);
    if (!m_socket->waitForConnected(timeout * 1000)) {
        emitError(QString("TCP 连接失败: %1").arg(m_socket->errorString()));
        return false;
    }

    // 把 socket fd 注入 libssh
    socket_t fd = m_socket->socketDescriptor();
    ssh_options_set(m_session, SSH_OPTIONS_FD, &fd);

    // 握手（密钥交换、认证协商）
    int rc = ssh_connect(m_session);
    if (rc != SSH_OK) {
        emitError(QString("SSH 握手失败: %1").arg(ssh_get_error(m_session)));
        return false;
    }

    // 认证
    if (!params.privateKeyPath.isEmpty()) {
        // 公钥认证（libssh 0.11+ 推荐 ssh_userauth_publickey_file 或 _auto）
        // 先尝试带 passphrase 模式
        ssh_key key = nullptr;
        QFileInfo ki(params.privateKeyPath);
        rc = ssh_pki_import_privkey_file(
            ki.absoluteFilePath().toLocal8Bit().constData(),
            nullptr,  // passphrase（无）
            nullptr, nullptr, &key);
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

    // 打开 shell 通道
    m_channel = ssh_channel_new(m_session);
    if (!m_channel) {
        emitError("无法创建 shell 通道");
        return false;
    }

    rc = ssh_channel_open_session(m_channel);
    if (rc != SSH_OK) {
        emitError(QString("打开 shell 通道失败: %1").arg(ssh_get_error(m_session)));
        return false;
    }

    // 请求 PTY
    rc = ssh_channel_request_pty_size(m_channel, "xterm",
        params.termCols, params.termRows);
    if (rc != SSH_OK) {
        emitError(QString("请求 PTY 失败: %1").arg(ssh_get_error(m_session)));
        return false;
    }

    // 启动 shell
    rc = ssh_channel_request_shell(m_channel);
    if (rc != SSH_OK) {
        emitError(QString("启动 shell 失败: %1").arg(ssh_get_error(m_session)));
        return false;
    }

    return true;
}

void SSHWorker::doDisconnect() {
    if (m_channel) {
        ssh_channel_close(m_channel);
        ssh_channel_free(m_channel);
        m_channel = nullptr;
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

void SSHWorker::doWrite(const QByteArray& data) {
    if (!m_channel) return;
    const char* ptr = data.constData();
    size_t remaining = static_cast<size_t>(data.size());
    while (remaining > 0) {
        int rc = ssh_channel_write(m_channel, ptr, remaining);
        if (rc < 0) {
            if (rc == SSH_AGAIN) {
                msleep(10);
                continue;
            }
            emitError("写入失败");
            return;
        }
        ptr += rc;
        remaining -= static_cast<size_t>(rc);
    }
}

void SSHWorker::doResize(int cols, int rows) {
    if (m_channel) {
        ssh_channel_request_pty_size(m_channel, "xterm", cols, rows);
    }
}

int SSHWorker::pumpRead() {
    if (!m_channel || !m_socket) return 0;

    char buf[8192];
    int n = ssh_channel_read_nonblocking(m_channel, buf, sizeof(buf), 0);
    if (n > 0) {
        emit dataReceived(QByteArray(buf, n));
        return n;
    }
    if (n == SSH_ERROR) {
        emitError("读取错误");
        return -1;
    }
    if (ssh_channel_is_eof(m_channel)) {
        emit statusMessage("远端关闭了 shell 通道");
        return -1;
    }
    return 0;
}

void SSHWorker::processQueue() {
    QMutexLocker locker(&m_mutex);
    while (!m_queue.isEmpty()) {
        const auto& cmd = m_queue.dequeue();
        switch (cmd.type) {
            case SSHClient::Command::Disconnect:
                m_running = false;
                return;
            case SSHClient::Command::Write:
                doWrite(cmd.data);
                break;
            case SSHClient::Command::Resize:
                doResize(cmd.cols, cmd.rows);
                break;
            case SSHClient::Command::Connect:
                // 已在 run() 中处理，忽略后续重复
                break;
        }
    }
}

void SSHWorker::emitError(const QString& context) {
    QString detail;
    if (m_session) {
        detail = QString::fromUtf8(ssh_get_error(m_session));
    } else {
        detail = "未知错误";
    }
    QString full = detail.isEmpty()
        ? context
        : QString("%1: %2").arg(context, detail);
    emit connectionError(full);
    Logger::instance().error(full, "ssh");
}