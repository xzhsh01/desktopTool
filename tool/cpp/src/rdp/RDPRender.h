#pragma once

#include <QObject>
#include <QWidget>
#include <QSize>

#include "rdp/RDPClient.h"

class QLabel;

/**
 * IRDPRender: 远程会话渲染后端抽象
 *
 * 每个远程会话窗口（RDPWindow）持有一种 IRDPRender 实现，负责把某个 RDP 会话
 * 渲染到 surface() 返回的控件上。切换后端不影响 RDPWidget / RDPWindow 的逻辑，
 * 由此实现「多窗口工作区」与「底层渲染」的解耦。
 *
 * 现役实现：
 *  - SystemRDPRender  调用系统原生客户端（mstsc / xfreerdp / rdesktop）
 *  - FreeRDPRender    （BR_WITH_FREERDP 时编译）FreeRDP 协议库内嵌渲染
 */
class IRDPRender : public QObject {
    Q_OBJECT
public:
    explicit IRDPRender(QObject* parent = nullptr) : QObject(parent) {}
    ~IRDPRender() override = default;

    // 启动/停止一个会话（params 由调用方持有引用，实现需要自行拷贝所需字段）
    virtual void start(const RDPClient::SessionParams& params) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    // 会话内容渲染表面控件（唯一真正显示远端桌面的控件）
    virtual QWidget* surface() = 0;

    // 该后端的人类可读名称（状态栏/标题栏显示）
    virtual QString backendName() const = 0;

    // 画面缩放模式（供嵌入式渲染表面使用）。模式："fit" / "original" / 百分比如 "50","100","150","200"。
    // 默认 no-op，系统客户端后端无需处理。
    virtual void setFrameScaleMode(const QString& mode) { Q_UNUSED(mode); }
    virtual QString frameScaleMode() const { return QString(); }

    // ── 会话信息查询（信息栏显示用；系统客户端后端返回空/0）──
    virtual QSize desktopSize() const { return QSize(); }   // 远端桌面分辨率
    virtual qint64 takeFrameBytes() { return 0; }           // 取走自上次调用以来收到的帧字节数（网速采样）

    // 在远端打开指定路径：注入 Win+R → Unicode 输入路径 → 回车。
    // 用于连接中修改共享文件夹后让远端立即打开（仅内嵌后端实现，连接中有效）
    virtual void openRemotePath(const QString& path) { Q_UNUSED(path); }

    // 注入低级键盘钩子拦截的系统级组合键（Win 键组合、Alt+Tab 等）。
    // 这些事件被 Windows 直接处理，Qt 收不到，由 KeyGrabber 钩子转发至此。
    // vk/scan 为 Win32 虚拟键码与 XT 扫描码；默认 no-op（系统客户端后端不适用）
    virtual void postHookedKey(int vk, int scan, bool extended, bool down) {
        Q_UNUSED(vk) Q_UNUSED(scan) Q_UNUSED(extended) Q_UNUSED(down)
    }

signals:
    void connected();                  // 会话已成功建立（到达登录/桌面）
    void error(const QString& message);// 连接/运行错误
    void finished(int exitCode);       // 会话结束
    void reconnecting(int attempt);    // 网络波动，正在自动重连（第 attempt 次）
    void authFailed();                 // 凭据被拒（密码错误/用户名不存在），请求弹框重输
};

// 工厂：返回当前平台可用的渲染后端（优先 FreeRDP 内嵌，否则系统客户端）
IRDPRender* createRDPRender(QObject* parent);

// 当前平台可用的渲染后端名称（空串表示完全不可用）
QString rdpRenderBackendName();