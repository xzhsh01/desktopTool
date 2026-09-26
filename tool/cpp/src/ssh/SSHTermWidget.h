#pragma once

#include <QWidget>
#include <QList>

#include "ssh/SSHClient.h"

class QTabWidget;
class TerminalEmulator;
class ConnectionManager;
class QTimer;

/**
 * SSHTermWidget: SSH 终端视图
 * 多标签终端：每个标签持有一个 SSHClient（内部封装 libssh + 工作线程）
 */
class SSHTermWidget : public QWidget {
    Q_OBJECT

public:
    explicit SSHTermWidget(QWidget* parent = nullptr);
    ~SSHTermWidget();

    // 打开（或激活）指定连接的终端会话
    void openSession(const QString& connectionId);

private slots:
    void onTabCloseRequested(int index);
    void onAddTabClicked();

private:
    void setupUI();
    void closeSession(int index);
    int findSessionIndex(const QString& connectionId) const;

    // SSHClient 信号回调
    void onSshConnected(const QString& connectionId);
    void onSshDisconnected(const QString& connectionId);
    void onSshData(const QString& connectionId, const QByteArray& data);
    void onSshError(const QString& connectionId, const QString& msg);

    // 会话：每个 tab 一个
    struct Session {
        QString connectionId;
        SSHClient* client = nullptr;   // owned via parent, deleted on close
        TerminalEmulator* terminal = nullptr;
        QWidget* container = nullptr;
        // ── 右键上传：当前目录探测状态 ──
        bool cwdProbeActive = false;   // 正在通过注入命令探测远端 cwd
        QString cwdProbeBuf;           // 探测期间累积的输出（用于提取路径）
        QString pendingUploadLocal;    // 探测完成后要上传的本地文件
        QString lastKnownCwd;          // 最近一次探测到的目录（菜单展示用）
        QTimer* cwdProbeTimer = nullptr; // 探测超时（回退到主目录）
    };

    void showTermContextMenu(TerminalEmulator* terminal, const QPoint& pos);
    void startUploadToCwd(int sessionIndex);
    void onCwdProbeData(int sessionIndex, const QByteArray& data);
    void onCwdProbeTimeout(int sessionIndex);
    void startUpload(int sessionIndex, const QString& remoteDir, const QString& localFile);
    static QString humanSize(qint64 bytes);

    QTabWidget* m_tabs = nullptr;
    QList<Session> m_sessions;
};