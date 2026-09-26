#pragma once

#include <QObject>
#include <QProcess>

/**
 * RDPClient: RDP 远程桌面客户端
 * 对应原 electron/rdp.ts
 *
 * 策略：调用系统原生 RDP 客户端
 *  - Windows: mstsc.exe（仅支持 /v: /w: /h: /f: /g: /admin 等开关；用户/密码通过 cmdkey 注入）
 *  - Linux: xfreerdp / rdesktop（若安装）
 *  - macOS: Microsoft Remote Desktop（通过 open 命令）
 */
class RDPClient : public QObject {
    Q_OBJECT

public:
    struct SessionParams {
        QString host;
        int port = 3389;
        QString username;
        QString password;
        QString domain;
        int width = 1280;
        int height = 800;
        bool fullscreen = false;       // /f 全屏模式
        QString gateway;               // /g:<gateway> RD 网关服务器
        bool admin = false;            // /admin 管理会话
        bool publicMode = false;       // /public 公共模式（不缓存凭据/位图）
        bool multiMon = false;         // /multimon 多显示器
        QString shellDir;              // 登录后自动打开的共享文件夹（\\tsclient\<盘>，空=默认本机系统盘共享根）
        bool prompt = false;           // /prompt 连接时提示输入凭据
        bool restrictedAdmin = false;  // /restrictedAdmin 受限管理模式（隐含 /admin）
        bool remoteGuard = false;      // /remoteGuard 远程防护（隐含 /admin，与 restrictedAdmin 互斥）
        QString extraArgs;    // 额外命令行参数
    };

    explicit RDPClient(QObject* parent = nullptr);

    // 检测可用的 RDP 客户端
    static bool isSupported();
    static QString clientName();

    // 启动 RDP 会话（异步，返回启动是否成功）
    bool startSession(const SessionParams& params);

    // 生成 .rdp 连接文件内容（Windows mstsc 备选方案）
    static QString generateRdpFile(const SessionParams& params);

signals:
    void sessionStarted();
    void sessionFinished(int exitCode);
    void sessionError(const QString& message);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    // 通过 cmdkey 注入/清理 Windows 凭据管理器中的 TERMSRV/<host> 条目，
    // 使已保存的密码能自动填入 mstsc 登录框（mstsc 命令行不支持 /u /d /p）。
    // target 仅用纯主机名（不带端口），mstsc 按此规则查找凭据。
    void injectCredentials(const QString& host, const QString& user,
                           const QString& domain, const QString& password);
    void clearInjectedCredentials();

    QProcess* m_process = nullptr;
    QString   m_credHost;   // 已注入凭据的目标主机（空表示未注入）
    QString   m_credUser;   // 已注入的用户名
};
