#pragma once

#include <QObject>
#include <QWidget>
#include <QString>
#include <QImage>

#include "rdp/RDPRender.h"
#include "rdp/RDPClient.h"

#ifdef BR_WITH_FREERDP
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#endif

/**
 * FreeRDPRender: 基于 FreeRDP 协议库的内嵌渲染后端。
 *
 * 仅在 CMake 传入 -DBR_WITH_FREERDP=ON 且找到 FreeRDP 库时编译本类
 * （见 CMakeLists.txt 的 freerdp 可选块与 scripts/setup_freerdp.ps1）。
 * 未启用时本文件保留类声明与占位实现，不影响构建。
 *
 * 接入思路（本文件已给出骨架与挂钩点）：
 *  - start() 在后台线程创建 rdp_context、填充设置、freerdp_connect 并泵事件循环；
 *  - 图像回调（BeginPaint/EndPaint，GDI 或 GFX 管线）把解码帧写入 m_frame，
 *    再通过 QMetaObject::invokeMethod 让 surface() 在 UI 线程重绘；
 *  - surface() 返回的 FreeRDPFrameSurface 由 paintEvent 把当前帧画到窗口。
 */
class FreeRDPRender final : public IRDPRender {
    Q_OBJECT
public:
    explicit FreeRDPRender(QObject* parent = nullptr);
    ~FreeRDPRender() override;

    void start(const RDPClient::SessionParams& params) override;
    void stop() override;
    bool isRunning() const override;
    QWidget* surface() override;
    QString backendName() const override;
    void setFrameScaleMode(const QString& mode) override;
    QString frameScaleMode() const override;

    // 会话信息（信息栏显示）：远端分辨率 / 网速采样
    QSize desktopSize() const override;
    qint64 takeFrameBytes() override;

    // 静态探测：编译期可用性 + 运行时库是否就绪
    static bool isAvailable();
    static QString backendNameStatic();

    // 供 frame surface 读取最近一帧（线程安全）
    QImage currentFrame() const;

    // 最近一次连接错误（空串=无错误；仅在主线程写入/读取，surface paintEvent 用）
    QString lastError() const;

    // 供 frame surface 向远端注入输入（线程安全，UI 线程调用；由 RDP 线程统一发送）
    void postMouse(int flags, int x, int y);
    void postKey(bool down, bool repeat, int rdpScancode);

    // 在远端打开指定路径（Win+R → Unicode 输入 → 回车；连接中有效）
    void openRemotePath(const QString& path) override;

    // 低级键盘钩子拦截的系统级组合键（Win/Alt+Tab）注入远端
    void postHookedKey(int vk, int scan, bool extended, bool down) override;

#ifdef BR_WITH_FREERDP
    // 供 FreeRDP EndPaint 回调（C 静态函数）写入新帧并请求 surface 重绘
    void deliverFrame(const QImage& frame);
#endif

private:
    struct InputEvent { int type; int a; int b; };
    // type: 0=mouse(a=flags,b=xy紧凑), 1=key(a=down|repeat<<1,b=scancode),
    //       2=unicode键(a=1按下/0释放,b=UTF-16码元), 3=延时(a=毫秒)

#ifdef BR_WITH_FREERDP
    // 后台 RDP 事件循环线程主体：freerdp_connect + 泵事件 + GDI 帧捕获
    void rdpLoop(const RDPClient::SessionParams& params);
    void requestSurfaceUpdate();
    // 取走待发送的输入事件（RDP 线程调用）
    std::vector<InputEvent> takeInput();
#endif

    RDPClient::SessionParams m_params;
    QWidget* m_surface = nullptr;
    bool m_running = false;

#ifdef BR_WITH_FREERDP
    QImage m_frame;                    // 受 m_frameMutex 保护
    mutable std::mutex m_frameMutex;
    QSize m_desktopSize;               // 远端桌面分辨率（受 m_frameMutex 保护）
    std::thread m_thread;              // RDP 连接/事件循环线程
    std::atomic<bool> m_keepRunning{false};
    QString m_error;
    QString m_scaleMode = QStringLiteral("fit");

    std::atomic<qlonglong> m_frameBytes{0};   // 收到的帧字节累计（网速采样用）
    int m_reconnectAttempt = 0;        // 当前重连序号（RDP 线程访问）

    std::vector<InputEvent> m_pendingInput;   // 待发送输入，受 m_inputMutex 保护
    std::mutex m_inputMutex;
#endif
};