#include "ssh/SSHTermWidget.h"
#include "ssh/TerminalEmulator.h"
#include "ssh/SFTPClient.h"
#include "connections/ConnectionManager.h"
#include "core/Logger.h"
#include "app/Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QInputDialog>
#include <QMenu>
#include <QFileDialog>
#include <QRegularExpression>
#include <QFileInfo>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>

// ════════════════════════════════════════════════════════════════════════════════
// SSHTermWidget
// ════════════════════════════════════════════════════════════════════════════════

SSHTermWidget::SSHTermWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
}

SSHTermWidget::~SSHTermWidget() {
    for (auto& s : m_sessions) {
        if (s.client) {
            s.client->disconnect();
        }
    }
}

void SSHTermWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tabs = new QTabWidget;
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &SSHTermWidget::onTabCloseRequested);

    // 空状态提示
    auto* emptyWidget = new QWidget;
    auto* emptyLayout = new QVBoxLayout(emptyWidget);
    emptyLayout->setAlignment(Qt::AlignCenter);
    auto* emptyLabel = new QLabel("暂无 SSH 会话\n\n从「应用管理」或「仪表盘」连接 SSH 服务器");
    emptyLabel->setAlignment(Qt::AlignCenter);
    emptyLabel->setStyleSheet(Theme::faintText());
    emptyLayout->addWidget(emptyLabel);

    auto* newSessionBtn = new QPushButton("新建 SSH 会话");
    connect(newSessionBtn, &QPushButton::clicked, this, &SSHTermWidget::onAddTabClicked);
    emptyLayout->addWidget(newSessionBtn, 0, Qt::AlignCenter);

    m_tabs->addTab(emptyWidget, "欢迎");

    layout->addWidget(m_tabs);
}

void SSHTermWidget::openSession(const QString& connectionId) {
    // 已有会话则激活
    int existing = findSessionIndex(connectionId);
    if (existing >= 0) {
        m_tabs->setCurrentIndex(existing);
        return;
    }

    auto* conn = ConnectionManager::instance().getById(connectionId);
    if (!conn) return;

    // 构造 SSHClient 连接参数
    SSHClient::ConnectParams params;
    params.host = conn->host;
    params.port = conn->port;
    params.username = conn->username;
    params.timeoutSec = 30;
    params.termCols = 80;
    params.termRows = 24;
    if (conn->authType == "key") {
        params.privateKeyPath = conn->privateKey;
    } else {
        params.password = conn->password;
    }

    // 创建终端 + SSHClient
    auto* terminal = new TerminalEmulator;
    auto* client = new SSHClient(this);

    // cwd 探测超时定时器（每会话一个）
    auto* probeTimer = new QTimer(this);
    probeTimer->setSingleShot(true);
    probeTimer->setInterval(5000);
    connect(probeTimer, &QTimer::timeout, this, [this, connectionId]() {
        int idx = findSessionIndex(connectionId);
        if (idx >= 0) onCwdProbeTimeout(idx);
    });

    Session session;
    session.connectionId = connectionId;
    session.client = client;
    session.terminal = terminal;
    session.cwdProbeTimer = probeTimer;

    // 终端输入 → SSHClient 写入
    connect(terminal, &TerminalEmulator::inputData, client, &SSHClient::write);
    // 终端尺寸变化 → 调整 PTY 大小
    connect(terminal, &TerminalEmulator::resized, this,
        [client](int cols, int rows) {
            client->resizeTerminal(cols, rows);
        });

    // 终端右键菜单（上传文件到当前目录）
    terminal->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(terminal, &QWidget::customContextMenuRequested, this,
        [this, terminal](const QPoint& pos) {
            showTermContextMenu(terminal, pos);
        });

    // SSHClient 信号 → UI
    connect(client, &SSHClient::connected, this,
        [this, connectionId]() { onSshConnected(connectionId); });
    connect(client, &SSHClient::disconnected, this,
        [this, connectionId]() { onSshDisconnected(connectionId); });
    connect(client, &SSHClient::dataReceived, this,
        [this, connectionId](const QByteArray& data) { onSshData(connectionId, data); });
    connect(client, &SSHClient::connectionError, this,
        [this, connectionId](const QString& msg) { onSshError(connectionId, msg); });

    QString tabTitle = conn->name;

    // 组装标签页
    auto* container = new QWidget;
    auto* cl = new QVBoxLayout(container);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(terminal);

    // 移除欢迎页（首次创建会话时）
    if (m_tabs->count() == 1 && m_sessions.isEmpty()) {
        m_tabs->removeTab(0);
    }

    int idx = m_tabs->addTab(container, tabTitle);
    m_tabs->setCurrentIndex(idx);
    session.container = container;

    m_sessions.append(session);

    // 发起连接
    terminal->write(QString("\x1b[90m正在连接 %1:%2...\x1b[0m\r\n")
        .arg(conn->host).arg(conn->port).toUtf8());
    client->connectTo(params);
    ConnectionManager::instance().setActive(connectionId, true);
    Logger::instance().info(QString("SSH 连接请求: %1@%2:%3")
        .arg(conn->username, conn->host).arg(conn->port), "ssh");

    terminal->setFocus();
}

void SSHTermWidget::onSshConnected(const QString& connectionId) {
    auto* conn = ConnectionManager::instance().getById(connectionId);
    QString tabTitle = conn ? conn->name : QStringLiteral("SSH");
    Logger::instance().success(QString("SSH 会话已建立: %1").arg(tabTitle), "ssh");
    // PTY/Shell 已在 SSHClient::doConnect 中请求，此处无需额外动作
    // 更新 tab 标题（去掉"连接中"等）
}

void SSHTermWidget::onSshDisconnected(const QString& connectionId) {
    auto it = std::find_if(m_sessions.begin(), m_sessions.end(),
        [&](const Session& s) { return s.connectionId == connectionId; });
    if (it != m_sessions.end() && it->terminal) {
        it->terminal->write("\r\n\x1b[33m[连接已断开]\x1b[0m\r\n");
    }
    ConnectionManager::instance().setActive(connectionId, false);
}

void SSHTermWidget::onSshData(const QString& connectionId, const QByteArray& data) {
    int idx = findSessionIndex(connectionId);
    if (idx < 0) return;
    Session& s = m_sessions[idx];
    if (s.terminal) {
        s.terminal->write(data);
    }
    if (s.cwdProbeActive) {
        onCwdProbeData(idx, data);
    }
}

void SSHTermWidget::onSshError(const QString& connectionId, const QString& msg) {
    auto it = std::find_if(m_sessions.begin(), m_sessions.end(),
        [&](const Session& s) { return s.connectionId == connectionId; });
    if (it != m_sessions.end() && it->terminal) {
        it->terminal->write(QString("\r\n\x1b[31m[连接错误] %1\x1b[0m\r\n").arg(msg).toUtf8());
    }
    Logger::instance().error(QString("SSH 错误: %1").arg(msg), "ssh");
    ConnectionManager::instance().setActive(connectionId, false);
}

void SSHTermWidget::onTabCloseRequested(int index) {
    closeSession(index);
}

void SSHTermWidget::onAddTabClicked() {
    auto sshConns = ConnectionManager::instance().getByType(ConnectionManager::SSH);
    if (sshConns.isEmpty()) {
        QMessageBox::information(this, "提示", "没有可用的 SSH 连接。\n请先在「应用管理」中创建。");
        return;
    }

    QStringList names;
    for (const auto& c : sshConns) names << c.name;

    bool ok = false;
    QString selected = QInputDialog::getItem(this, "新建 SSH 会话",
        "选择连接:", names, 0, false, &ok);
    if (!ok || selected.isEmpty()) return;

    for (const auto& c : sshConns) {
        if (c.name == selected) {
            openSession(c.id);
            break;
        }
    }
}

void SSHTermWidget::closeSession(int index) {
    if (index < 0 || index >= m_sessions.size()) return;

    Session& s = m_sessions[index];
    if (s.cwdProbeTimer) s.cwdProbeTimer->stop();
    s.cwdProbeActive = false;
    if (s.client) {
        s.client->disconnect();
        s.client->deleteLater();
    }
    ConnectionManager::instance().setActive(s.connectionId, false);
    m_tabs->removeTab(index);
    m_sessions.removeAt(index);

    // 全部关闭后恢复欢迎页
    if (m_sessions.isEmpty()) {
        auto* emptyWidget = new QWidget;
        auto* emptyLayout = new QVBoxLayout(emptyWidget);
        emptyLayout->setAlignment(Qt::AlignCenter);
        auto* emptyLabel = new QLabel("暂无 SSH 会话\n\n从「应用管理」或「仪表盘」连接 SSH 服务器");
        emptyLabel->setAlignment(Qt::AlignCenter);
        emptyLabel->setStyleSheet(Theme::faintText());
        emptyLayout->addWidget(emptyLabel);

        auto* newSessionBtn = new QPushButton("新建 SSH 会话");
        connect(newSessionBtn, &QPushButton::clicked, this, &SSHTermWidget::onAddTabClicked);
        emptyLayout->addWidget(newSessionBtn, 0, Qt::AlignCenter);

        m_tabs->addTab(emptyWidget, "欢迎");
    }
}

int SSHTermWidget::findSessionIndex(const QString& connectionId) const {
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions[i].connectionId == connectionId) return i;
    }
    return -1;
}

// ════════════════════════════════════════════════════════════════════════════════
// 终端右键上传（SFTP 上传到远端当前目录，带进度显示）
// ════════════════════════════════════════════════════════════════════════════════

void SSHTermWidget::showTermContextMenu(TerminalEmulator* terminal, const QPoint& pos) {
    // 按终端指针定位会话
    int idx = -1;
    for (int i = 0; i < m_sessions.size(); ++i) {
        if (m_sessions[i].terminal == terminal) { idx = i; break; }
    }
    if (idx < 0) return;
    Session& s = m_sessions[idx];

    QMenu menu(this);
    auto* conn = ConnectionManager::instance().getById(s.connectionId);
    if (conn) {
        auto* title = menu.addAction(conn->name);
        title->setEnabled(false);
        menu.addSeparator();
    }
    if (!s.lastKnownCwd.isEmpty()) {
        auto* cwdInfo = menu.addAction(QStringLiteral("当前目录: %1").arg(s.lastKnownCwd));
        cwdInfo->setEnabled(false);
    }
    QAction* uploadAct = menu.addAction(QStringLiteral("上传文件到当前目录..."));
    uploadAct->setEnabled(s.client && s.client->isConnected() && !s.cwdProbeActive);

    QAction* chosen = menu.exec(terminal->mapToGlobal(pos));
    if (chosen == uploadAct) {
        startUploadToCwd(idx);
    }
}

void SSHTermWidget::startUploadToCwd(int sessionIndex) {
    if (sessionIndex < 0 || sessionIndex >= m_sessions.size()) return;
    Session& s = m_sessions[sessionIndex];
    if (!s.client || !s.client->isConnected() || s.cwdProbeActive) return;

    const QString local = QFileDialog::getOpenFileName(this, QStringLiteral("选择要上传的文件"));
    if (local.isEmpty()) return;

    // 提示（写入终端，便于用户理解接下来出现的探测命令）
    if (s.terminal) {
        s.terminal->write(QStringLiteral("\x1b[90m[上传] 正在获取远端当前目录...\x1b[0m\r\n").toUtf8());
    }

    // 进入探测状态：注入一条 echo 命令，从输出中提取 $PWD 展开值。
    // 标记通过变量拼接：回显行里只出现一个 __KFCWD__（成对正则不会误匹配回显），
    // 仅命令执行输出才包含完整的一对标记。
    s.cwdProbeActive = true;
    s.cwdProbeBuf.clear();
    s.pendingUploadLocal = local;
    if (s.cwdProbeTimer) s.cwdProbeTimer->start();
    s.client->write(QStringLiteral(" M=__KFCWD__; echo \"$M$PWD$M\"\r").toUtf8());
}

void SSHTermWidget::onCwdProbeData(int sessionIndex, const QByteArray& data) {
    if (sessionIndex < 0 || sessionIndex >= m_sessions.size()) return;
    Session& s = m_sessions[sessionIndex];

    s.cwdProbeBuf += QString::fromUtf8(data);
    if (s.cwdProbeBuf.size() > 8192) {
        s.cwdProbeBuf = s.cwdProbeBuf.right(4096);
    }

    // 提取 __KFCWD__...__KFCWD__（回显行只含单个标记，不会误匹配）
    static const QRegularExpression re(QStringLiteral("__KFCWD__([^\\r\\n]*)__KFCWD__"));
    auto m = re.match(s.cwdProbeBuf);
    if (m.hasMatch()) {
        const QString raw = m.captured(1).trimmed();
        // 校验并规范化为 SFTP 可用路径：POSIX 绝对路径或 Windows 盘符路径
        QString cwd;
        if (raw.startsWith(QLatin1Char('/'))) {
            cwd = raw;
        } else if (raw.size() >= 2 && raw.at(1) == QLatin1Char(':')) {
            cwd = raw;
            cwd.replace(QLatin1Char('\\'), QLatin1Char('/'));   // C:\x → C:/x
        }
        if (!cwd.isEmpty()) {
            s.cwdProbeActive = false;
            s.cwdProbeBuf.clear();
            if (s.cwdProbeTimer) s.cwdProbeTimer->stop();
            s.lastKnownCwd = cwd;
            startUpload(sessionIndex, cwd, s.pendingUploadLocal);
            return;
        }
        // 假匹配（如回显重复拼接）：丢弃该片段，继续等待真实输出
        s.cwdProbeBuf.remove(0, m.capturedEnd(0));
    }
}

void SSHTermWidget::onCwdProbeTimeout(int sessionIndex) {
    if (sessionIndex < 0 || sessionIndex >= m_sessions.size()) return;
    Session& s = m_sessions[sessionIndex];
    if (!s.cwdProbeActive) return;

    s.cwdProbeActive = false;
    s.pendingUploadLocal.clear();
    // 记录探测期间终端输出片段（去除 ANSI 转义），便于定位探测失败原因
    QString clean = s.cwdProbeBuf;
    clean.remove(QRegularExpression("\x1b\\[[0-9;?]*[a-zA-Z]"));
    clean.replace('\r', ' ').replace('\n', ' ');
    s.cwdProbeBuf.clear();
    Logger::instance().warn(QStringLiteral(
        "cwd 探测超时, 终端输出片段: %1").arg(clean.left(300)), "sftp");
    if (s.terminal) {
        s.terminal->write(QStringLiteral(
            "\x1b[31m[上传失败] 未能获取远端当前目录（请确保终端处于 shell 提示符下），已取消上传\x1b[0m\r\n").toUtf8());
    }
}

QString SSHTermWidget::humanSize(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024) return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 2);
}

void SSHTermWidget::startUpload(int sessionIndex, const QString& remoteDir,
                                const QString& localFile) {
    if (sessionIndex < 0 || sessionIndex >= m_sessions.size()) return;
    Session& s = m_sessions[sessionIndex];
    auto conn = ConnectionManager::instance().getById(s.connectionId);
    if (!conn || !s.terminal) return;

    QFileInfo fi(localFile);
    const QString fileName = fi.fileName();
    QString remotePath;
    if (remoteDir.isEmpty()) {
        remotePath = fileName;   // SFTP 会话默认目录即用户主目录
    } else {
        remotePath = remoteDir.endsWith('/') ? remoteDir + fileName
                                             : remoteDir + "/" + fileName;
    }

    // SFTP 连接参数（与终端会话一致）
    SFTPClient::ConnectParams params;
    params.host = conn->host;
    params.port = conn->port;
    params.username = conn->username;
    params.timeoutSec = 30;
    if (conn->authType == "key") {
        params.privateKeyPath = conn->privateKey;
    } else {
        params.password = conn->password;
    }

    auto* sftp = new SFTPClient(this);

    // 结束标志（防止 transferFinished / connectionError 重复触发）
    auto finished = std::make_shared<bool>(false);
    auto speedTimer = std::make_shared<QElapsedTimer>();

    auto cleanup = [sftp]() { sftp->deleteLater(); };
    // 注意：不直接捕获 Session&（会话列表可能重分配），按 connectionId 现查
    const QString connId = s.connectionId;
    auto termMsg = [this, connId](const QString& colored) {
        int idx = findSessionIndex(connId);
        if (idx >= 0 && m_sessions[idx].terminal) {
            m_sessions[idx].terminal->write(colored.toUtf8());
        }
    };

    connect(sftp, &SFTPClient::connected, this, [sftp, localFile, remotePath, fileName,
                                                  speedTimer, termMsg]() {
        speedTimer->start();
        termMsg(QStringLiteral("\x1b[90m[上传] 开始: %1 → %2\x1b[0m\r\n")
                    .arg(fileName, remotePath));
        sftp->upload(localFile, remotePath);
    });

    // 进度直接输出到终端：\r 回行首 + \x1b[K 清行，原地刷新同一行
    connect(sftp, &SFTPClient::uploadProgress, this,
            [termMsg, speedTimer](const QString& /*path*/, qint64 done, qint64 total) {
        double secs = speedTimer->elapsed() / 1000.0;
        const QString speed = secs > 0.2
            ? QStringLiteral(" | %1/s").arg(SSHTermWidget::humanSize(qint64(done / secs)))
            : QString();
        QString line;
        if (total > 0) {
            const int pct = static_cast<int>(qBound<qint64>(0LL, done * 100 / total, 100LL));
            line = QStringLiteral("\r\x1b[K\x1b[90m[上传] %1% (%2 / %3)%4\x1b[0m")
                       .arg(pct).arg(SSHTermWidget::humanSize(done),
                                     SSHTermWidget::humanSize(total), speed);
        } else {
            line = QStringLiteral("\r\x1b[K\x1b[90m[上传] 已发送 %1%2\x1b[0m")
                       .arg(SSHTermWidget::humanSize(done), speed);
        }
        termMsg(line);
    });

    connect(sftp, &SFTPClient::transferFinished, this,
            [this, cleanup, finished, remotePath, fileName, termMsg]
            (const QString& /*path*/, bool success, const QString& error) {
        if (*finished) return;
        *finished = true;
        cleanup();
        if (success) {
            // 输出文件所在远端路径
            termMsg(QStringLiteral(
                "\r\x1b[K\x1b[32m[上传完成] 文件位于: %1\x1b[0m\r\n").arg(remotePath));
            Logger::instance().success(
                QStringLiteral("终端上传完成: %1 → %2").arg(fileName, remotePath), "sftp");
        } else if (error == QStringLiteral("已取消")) {
            termMsg(QStringLiteral("\r\x1b[K\x1b[33m[上传已取消] %1\x1b[0m\r\n").arg(fileName));
        } else {
            termMsg(QStringLiteral(
                "\r\x1b[K\x1b[31m[上传失败] %1: %2\x1b[0m\r\n").arg(fileName, error));
            Logger::instance().error(
                QStringLiteral("终端上传失败: %1 (%2)").arg(fileName, error), "sftp");
        }
    });

    connect(sftp, &SFTPClient::connectionError, this,
            [this, cleanup, finished, fileName, termMsg](const QString& msg) {
        if (*finished) return;
        *finished = true;
        cleanup();
        termMsg(QStringLiteral(
            "\r\x1b[K\x1b[31m[上传失败] %1: %2\x1b[0m\r\n").arg(fileName, msg));
    });

    sftp->connectTo(params);
}