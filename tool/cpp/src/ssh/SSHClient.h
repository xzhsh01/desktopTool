#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QString>
#include <QQueue>
#include <QByteArray>

// libssh C API（不暴露在头文件中：所有 libssh 类型对调用方隐藏）
struct ssh_session_struct;
typedef struct ssh_session_struct* ssh_session;
struct ssh_channel_struct;
typedef struct ssh_channel_struct* ssh_channel;

class SSHWorker;

/**
 * SSHClient: SSH 终端客户端（libssh 封装）
 *
 * 设计：所有 libssh 调用在专用工作线程中执行（libssh 默认阻塞，且非线程安全），
 * 主线程通过信号槽（队列连接）通信。
 *
 * 与 libssh2 版本接口兼容——便于 SSHTermWidget 继续使用。
 */
class SSHClient : public QObject {
    Q_OBJECT

public:
    // 连接参数
    struct ConnectParams {
        QString host;
        int port = 22;
        QString username;
        QString password;
        QString privateKeyPath;   // 为空则密码认证；非空则尝试公钥
        int timeoutSec = 30;
        int termCols = 80;
        int termRows = 24;
    };

    // 内部命令
    struct Command {
        enum Type { Connect, Disconnect, Write, Resize } type;
        QByteArray data;          // Write: 负载
        ConnectParams params;     // Connect
        int cols = 0, rows = 0;   // Resize
    };

    explicit SSHClient(QObject* parent = nullptr);
    ~SSHClient();

    // 公共 API（线程安全：命令入队，由工作线程执行）
    void connectTo(const ConnectParams& params);
    void disconnect();
    void write(const QByteArray& data);
    void resizeTerminal(int cols, int rows);

    bool isConnected() const { return m_connected; }

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& message);
    // 终端输出数据
    void dataReceived(const QByteArray& data);
    // 连接状态文本（供 UI 显示）
    void statusMessage(const QString& message);

private:
    void setConnected(bool v);
    SSHWorker* m_worker = nullptr;
    bool m_connected = false;
};

/**
 * SSHWorker: 工作线程
 * run() 中执行：连接 → 认证 → 打开 shell → 读写循环
 */
class SSHWorker : public QThread {
    Q_OBJECT

public:
    explicit SSHWorker(QObject* parent = nullptr);
    ~SSHWorker();

    void queueCommand(const SSHClient::Command& cmd);
    void requestStop();

signals:
    void connected();
    void disconnected();
    void connectionError(const QString& message);
    void dataReceived(const QByteArray& data);
    void statusMessage(const QString& message);

protected:
    void run() override;

private:
    bool doConnect(const SSHClient::ConnectParams& params);
    void doDisconnect();
    void doWrite(const QByteArray& data);
    void doResize(int cols, int rows);
    int pumpRead();
    void emitError(const QString& context);
    void processQueue();      // 在 pumpRead 间隙处理待发命令
    bool isSocketReadable(int timeoutMs);  // 用 QSocketNotifier 无法跨线程，自己 poll

    SSHClient::ConnectParams m_params;

    // libssh 句柄（仅工作线程访问）
    ssh_session m_session = nullptr;
    ssh_channel m_channel = nullptr;
    class QTcpSocket* m_socket = nullptr;   // 用于 select() 等待 socket 可读
    bool m_running = false;

    // 命令队列
    QMutex m_mutex;
    QQueue<SSHClient::Command> m_queue;
};