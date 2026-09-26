#include "rdp/RDPClient.h"
#include "core/Logger.h"

#include <QFileInfo>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>

RDPClient::RDPClient(QObject* parent) : QObject(parent) {}

bool RDPClient::isSupported() {
#ifdef Q_OS_WIN
    return QFileInfo::exists("C:/Windows/System32/mstsc.exe");
#elif defined(Q_OS_LINUX)
    // xfreerdp 或 rdesktop
    return !QStandardPaths::findExecutable("xfreerdp").isEmpty() ||
           !QStandardPaths::findExecutable("rdesktop").isEmpty();
#elif defined(Q_OS_MAC)
    return QFileInfo::exists("/Applications/Microsoft Remote Desktop.app");
#else
    return false;
#endif
}

QString RDPClient::clientName() {
#ifdef Q_OS_WIN
    return "mstsc";
#elif defined(Q_OS_LINUX)
    if (!QStandardPaths::findExecutable("xfreerdp").isEmpty()) return "xfreerdp";
    if (!QStandardPaths::findExecutable("rdesktop").isEmpty()) return "rdesktop";
    return {};
#elif defined(Q_OS_MAC)
    return "Microsoft Remote Desktop";
#else
    return {};
#endif
}

bool RDPClient::startSession(const SessionParams& params) {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        emit sessionError("已有 RDP 会话正在运行");
        return false;
    }

    if (m_process) {
        m_process->deleteLater();
    }
    m_process = new QProcess(this);

    connect(m_process, &QProcess::finished, this, &RDPClient::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &RDPClient::onProcessError);

    QString address = params.host;
    if (params.port != 3389) {
        address += QString(":%1").arg(params.port);
    }

#ifdef Q_OS_WIN
    // mstsc.exe：只接受命令行开关（/v /w /h /f /g /admin /public /multimon
    //            /restrictedAdmin /remoteGuard /prompt 等），不支持 /u /d /p。
    // 用户名/域/密码 通过 Windows 凭据管理器（cmdkey TERMSRV/<host>）注入。
    QStringList args;
    args << "/v:" + address;
    if (params.fullscreen) {
        args << "/f";
    } else {
        args << "/w:" + QString::number(params.width);
        args << "/h:" + QString::number(params.height);
    }
    if (!params.gateway.isEmpty()) {
        args << "/g:" + params.gateway;
    }
    // /restrictedAdmin 与 /remoteGuard 均隐含 /admin 且互斥，remoteGuard 优先
    if (params.remoteGuard) {
        args << "/remoteGuard";
    } else if (params.restrictedAdmin) {
        args << "/restrictedAdmin";
    } else if (params.admin) {
        args << "/admin";
    }
    if (params.publicMode) args << "/public";
    if (params.multiMon)   args << "/multimon";
    if (params.prompt)     args << "/prompt";
    if (!params.extraArgs.isEmpty()) {
        args << params.extraArgs;
    }

    // 注入已保存的凭据（仅当用户名与密码均非空时）
    // 注意：cmdkey target 必须是 TERMSRV/<host>（不带端口），mstsc 仅按此规则查找凭据
    if (!params.username.isEmpty() && !params.password.isEmpty()) {
        injectCredentials(params.host, params.username, params.domain, params.password);
    }

    Logger::instance().info(QString("启动 RDP 会话: mstsc %1").arg(args.join(' ')), "rdp");
    m_process->start("mstsc.exe", args);

#elif defined(Q_OS_LINUX)
    QString exe = QStandardPaths::findExecutable("xfreerdp");
    QStringList args;
    if (!exe.isEmpty()) {
        // xfreerdp
        args << "/v:" + address;
        if (!params.username.isEmpty()) args << "/u:" + params.username;
        if (!params.password.isEmpty()) args << "/p:" + params.password;
        if (!params.domain.isEmpty()) args << "/d:" + params.domain;
        args << QString("/size:%1x%2").arg(params.width).arg(params.height);
        if (params.fullscreen) args << "/f";
        args << "/cert:ignore";
        if (!params.extraArgs.isEmpty()) args << params.extraArgs;
        Logger::instance().info(QString("启动 RDP 会话: xfreerdp %1").arg(args.join(' ')), "rdp");
        m_process->start(exe, args);
    } else {
        exe = QStandardPaths::findExecutable("rdesktop");
        args << address;
        if (!params.username.isEmpty()) args << "-u" << params.username;
        if (!params.password.isEmpty()) args << "-p" << params.password;
        args << "-g" << QString("%1x%2").arg(params.width).arg(params.height);
        if (params.fullscreen) args << "-f";
        Logger::instance().info(QString("启动 RDP 会话: rdesktop %1").arg(args.join(' ')), "rdp");
        m_process->start(exe, args);
    }

#elif defined(Q_OS_MAC)
    // macOS: 通过 URL scheme
    QString url = QString("rdp://%1@%2").arg(params.username, address);
    QStringList args;
    args << "-a" << "Microsoft Remote Desktop" << url;
    Logger::instance().info(QString("启动 RDP 会话: open %1").arg(args.join(' ')), "rdp");
    m_process->start("open", args);
#else
    Q_UNUSED(params)
    Q_UNUSED(address)
    emit sessionError("当前平台不支持 RDP");
    return false;
#endif

    if (!m_process->waitForStarted(5000)) {
        emit sessionError("无法启动 RDP 客户端进程");
        return false;
    }

    Logger::instance().success(QString("RDP 会话已启动: %1").arg(address), "rdp");
    emit sessionStarted();
    return true;
}

void RDPClient::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status)
    Logger::instance().info(QString("RDP 会话结束 (exit=%1)").arg(exitCode), "rdp");
    clearInjectedCredentials();
    emit sessionFinished(exitCode);
}

void RDPClient::onProcessError(QProcess::ProcessError error) {
    QString msg;
    switch (error) {
        case QProcess::FailedToStart: msg = "RDP 客户端启动失败"; break;
        case QProcess::Crashed: msg = "RDP 客户端崩溃"; break;
        case QProcess::Timedout: msg = "RDP 客户端超时"; break;
        default: msg = "RDP 客户端错误"; break;
    }
    // 启动失败时也要清理可能已注入的凭据
    clearInjectedCredentials();
    emit sessionError(msg);
}

void RDPClient::injectCredentials(const QString& host, const QString& user,
                                  const QString& domain, const QString& password) {
#ifdef Q_OS_WIN
    // 凭据目标：TERMSRV/<host>（mstsc 只按此规则查找 Generic 凭据，不能带端口）
    // 若已存在旧条目，先删除避免冲突
    clearInjectedCredentials();

    // cmdkey /user 接受 "DOMAIN\user" 或 "user@domain" 形式 —— mstsc 弹出的登录框
    // 显示的就是该组合形式，可保证 NLA / CredSSP 协商时凭据匹配
    QString fullUser = user;
    if (!domain.isEmpty() && !user.contains('\\') && !user.contains('@')) {
        fullUser = domain + "\\" + user;
    }

    const QStringList args = {
        QString("/generic:TERMSRV/%1").arg(host),
        QString("/user:%1").arg(fullUser),
        QString("/pass:%1").arg(password)
    };
    const int rc = QProcess::execute("cmdkey", args);
    if (rc != 0) {
        Logger::instance().error(
            QString("cmdkey 注入凭据失败 (rc=%1): TERMSRV/%2 user=%3")
                .arg(rc).arg(host, fullUser), "rdp");
        emit sessionError(QString("无法写入 RDP 凭据 (cmdkey rc=%1)").arg(rc));
        return;
    }

    m_credHost = host;
    m_credUser = fullUser;
    Logger::instance().info(
        QString("已注入 RDP 凭据: TERMSRV/%1 (user=%2)").arg(host, fullUser), "rdp");
#else
    Q_UNUSED(host); Q_UNUSED(user); Q_UNUSED(domain); Q_UNUSED(password);
#endif
}

void RDPClient::clearInjectedCredentials() {
#ifdef Q_OS_WIN
    if (m_credHost.isEmpty()) return;
    // cmdkey /delete 静默执行，失败不报错（条目可能已被外部清理）
    QProcess::execute("cmdkey", QStringList() << QString("/delete:TERMSRV/%1").arg(m_credHost));
    Logger::instance().info(QString("已清理 RDP 凭据: TERMSRV/%1").arg(m_credHost), "rdp");
    m_credHost.clear();
    m_credUser.clear();
#endif
}

QString RDPClient::generateRdpFile(const SessionParams& params) {
    QString address = params.host;
    if (params.port != 3389) {
        address += QString(":%1").arg(params.port);
    }

    QString rdp;
    rdp += "screen mode id:i:" + QString(params.fullscreen ? "2" : "1") + "\n";
    if (!params.fullscreen) {
        rdp += QString("desktopwidth:i:%1\ndesktopheight:i:%2\n")
            .arg(params.width).arg(params.height);
    }
    rdp += "session bpp:i:32\n";
    rdp += "compression:i:1\n";
    rdp += "keyboardhook:i:2\n";
    rdp += "audiocapturemode:i:0\n";
    rdp += "videoplaybackmode:i:1\n";
    rdp += "connection type:i:7\n";
    rdp += "networkautodetect:i:1\n";
    rdp += "bandwidthautodetect:i:1\n";
    rdp += "displayconnectionbar:i:1\n";
    rdp += "disable wallpaper:i:0\n";
    rdp += "allow font smoothing:i:1\n";
    rdp += "allow desktop composition:i:1\n";
    rdp += "disable full window drag:i:0\n";
    rdp += "disable menu anims:i:0\n";
    rdp += "disable themes:i:0\n";
    rdp += "disable cursor setting:i:0\n";
    rdp += "bitmapcachepersistenable:i:1\n";
    rdp += "full address:s:" + address + "\n";
    if (!params.username.isEmpty()) {
        rdp += "username:s:" + params.username + "\n";
    }
    if (!params.domain.isEmpty()) {
        rdp += "domain:s:" + params.domain + "\n";
    }
    rdp += "alternate shell:s:\n";
    rdp += "shell working directory:s:\n";
    rdp += "authentication level:i:2\n";
    rdp += "prompt for credentials:i:" + QString(params.prompt ? "1" : "0") + "\n";
    rdp += "administrative session:i:" + QString((params.admin || params.restrictedAdmin || params.remoteGuard) ? "1" : "0") + "\n";
    rdp += "negotiate security layer:i:1\n";
    rdp += "remoteapplicationmode:i:0\n";
    rdp += "alternate full address:s:\n";
    rdp += "gatewayhostname:s:" + params.gateway + "\n";
    rdp += "gatewayusagemethod:i:4\n";
    rdp += "gatewaycredentialssource:i:4\n";
    rdp += "gatewayprofileusagemethod:i:0\n";
    rdp += "promptcredentialonce:i:0\n";
    rdp += "use redirection server name:i:0\n";
    return rdp;
}
