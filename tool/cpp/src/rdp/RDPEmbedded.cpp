// 静态链接 FreeRDP：必须在包含任何 FreeRDP 头之前定义 FREERDP_EXPORTS，
// 否则 FREERDP_API 变成 __declspec(dllimport)，链接静态库时产生 __imp_ 符号未解析。
// （winpr 侧无需处理：不定义 WINPR_DLL 时 WINPR_API 为空宏，直接引用符号。）
#ifdef BR_WITH_FREERDP
#define FREERDP_EXPORTS 1
#endif

#include "rdp/RDPEmbedded.h"

#include "app/Theme.h"
#include "core/Logger.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QPixmap>
#include <QCursor>
#include <QApplication>
#include <QInputDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef BR_WITH_FREERDP
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#include <freerdp/scancode.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/graphics.h>
#include <freerdp/codec/color.h>
#include <freerdp/error.h>
#include <freerdp/addin.h>             // freerdp_register_addin_provider
#include <freerdp/channels/channels.h> // CHANNEL_RC_OK
#include <freerdp/channels/cliprdr.h>  // CLIPRDR_SVC_CHANNEL_NAME
#include <freerdp/client/channels.h>   // freerdp_channels_load_static_addin_entry
#include <freerdp/client/cmdline.h>    // freerdp_client_load_addins
#include <freerdp/event.h>             // ChannelConnectedEventArgs（cliprdr 挂载点）
#include <freerdp/locale/keyboard.h>   // KBD_UNITED_STATES
#include <winpr/collections.h>         // PubSub_SubscribeChannelConnected
#include "rdp/BrCliprdrShim.h"         // 最小 wfContext + wf_cliprdr_init/uninit
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>                   // GetKeyState（本地锁定键状态同步）
#endif
#include <cstring>
#include <cstdio>
#include <chrono>
#include <algorithm>
#endif // BR_WITH_FREERDP

#ifdef BR_WITH_FREERDP

// ═══════════════════════════════════════════════════════════════════════
// FreeRDP 客户端上下文：首嵌 rdpContext，随 context 一起分配/释放
// ═══════════════════════════════════════════════════════════════════════
struct FreeRDPClientContext {
    wfContext wf;                          // 必须位于首部：freerdp_context_new 按
                                           // instance->ContextSize 分配并初始化其内嵌
                                           // rdpContext；clipboard 指针供 wf_cliprdr 使用
    FreeRDPRender* render = nullptr;
    RDPClient::SessionParams params;
    pEndPaint origEndPaint = nullptr;      // gdi_init 注册的原始 EndPaint
    pBitmapUpdate origBitmapUpdate = nullptr; // gdi 注册的原始 BitmapUpdate
    bool firstFrameSeen = false;           // 仅 RDP 线程访问，用于首帧日志
    void* cliprdrCtx = nullptr;            // ChannelConnected 时保存的 CliprdrClientContext*
    // 帧交付节流（仅 RDP 线程访问）：位图更新高频到达时合并为 ~30fps 交付
    std::chrono::steady_clock::time_point lastFrameCopy{};
    bool frameDirty = false;               // 节流窗口内有未交付的更新
};

static FreeRDPClientContext* asClientContext(rdpContext* ctx) {
    return reinterpret_cast<FreeRDPClientContext*>(ctx);
}

// ── 日志：RDP 线程 → 主线程（Logger 内部容器/文件写入统一切回主线程执行）──
static void rdpLogInfo(const QString& msg) {
    QMetaObject::invokeMethod(&Logger::instance(), [msg] {
        Logger::instance().info(msg, QStringLiteral("rdp"));
    }, Qt::QueuedConnection);
}
static void rdpLogError(const QString& msg) {
    QMetaObject::invokeMethod(&Logger::instance(), [msg] {
        Logger::instance().error(msg, QStringLiteral("rdp"));
    }, Qt::QueuedConnection);
}

// ── cliprdr 后端挂载 ─────────────────────────────────────────────────────
// cliprdr 通道插件只做协议收发，对接本机剪贴板的 Server* 回调表必须由客户端
// 后端填充；不挂载时 FreeRDP 对所有剪贴板 PDU 静默丢弃（回调 NULL 仅 Verbose
// 日志），本地↔远端复制粘贴（文本与文件）全部失效。这里在通道连接事件到来时
// 挂上 FreeRDP 官方 Windows 后端 wf_cliprdr（经 br_cliprdr_win.c 复用编译）。
static void br_OnChannelConnected(void* arg, const ChannelConnectedEventArgs* e) {
    auto* c = asClientContext(reinterpret_cast<rdpContext*>(arg));
    if (!c || std::strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) != 0)
        return;
    if (c->wf.clipboard)                   // 重连重复事件：已挂载
        return;
    if (wf_cliprdr_init(&c->wf, reinterpret_cast<CliprdrClientContext*>(e->pInterface))) {
        c->cliprdrCtx = e->pInterface;
        rdpLogInfo(QStringLiteral("剪贴板后端已挂载（文本/文件双向）"));
    } else {
        rdpLogError(QStringLiteral("剪贴板后端初始化失败"));
    }
}

// 卸载剪贴板后端（幂等）。wf_cliprdr_uninit 不清 wfc->clipboard，
// 必须手动置空，否则二次调用会对野指针 double free。
static void br_cliprdr_teardown(FreeRDPClientContext* c) {
    if (!c || !c->wf.clipboard)
        return;
    wf_cliprdr_uninit(&c->wf, reinterpret_cast<CliprdrClientContext*>(c->cliprdrCtx));
    c->wf.clipboard = nullptr;
    c->cliprdrCtx = nullptr;
}

// ── WLog 重定向：FreeRDP 内部日志 → 文件（GUI 无控制台，默认丢失）────────
// CredSSP/NLA 失败时 FreeRDP 会输出服务器返回的 NTSTATUS 详情（密码错/
// 账户锁定/禁用等精确原因），连接失败后由 readWlogTail 提取到日志页。
static const char* kWlogFileName = "bambooRat_freerdp.log";

static void ensureWlogFile() {
    static bool done = false;
    if (done)
        return;
    done = true;
    static char bufAppender[] = "WLOG_APPENDER=FILE";
    static char bufLevel[] = "WLOG_LEVEL=INFO";
    static char bufName[] = "WLOG_FILEAPPENDER_OUTPUT_FILE_NAME=bambooRat_freerdp.log";
    static char bufPath[512];
    std::snprintf(bufPath, sizeof(bufPath), "WLOG_FILEAPPENDER_OUTPUT_FILE_PATH=%s",
                  QDir::tempPath().toLocal8Bit().constData());
    _putenv(bufAppender);
    _putenv(bufLevel);
    _putenv(bufName);
    _putenv(bufPath);
    // 追加模式文件过大时轮转
    QFile f(QString::fromLocal8Bit(QDir::tempPath().toLocal8Bit()) + QLatin1Char('/') +
            QLatin1String(kWlogFileName));
    if (f.exists() && f.size() > 2 * 1024 * 1024)
        f.remove();
}

static QString readWlogTail(int maxLines = 50) {
    QFile f(QDir::tempPath() + QLatin1Char('/') + QLatin1String(kWlogFileName));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    const QList<QByteArray> lines = f.readAll().split('\n');
    QStringList tail;
    for (int i = qMax(0, lines.size() - maxLines); i < lines.size(); ++i) {
        const QString l = QString::fromUtf8(lines[i].trimmed());
        if (!l.isEmpty())
            tail << l;
    }
    return tail.join(QLatin1Char('\n'));
}

// ── 凭据格式兼容：mstsc 风格 "域\用户" / ".\用户" 自动拆分 ────────────────
// FreeRDP 的 NTLM/CredSSP 不会拆分用户名中的 '\'，整串发送会被服务器拒绝
// （Logon failed）。与 mstsc 对齐：用户名里的域前缀优先；"user@upn" 保持原样。
static void splitUserDomain(QString& user, QString& domain) {
    const int bs = user.lastIndexOf(QLatin1Char('\\'));
    if (bs >= 0) {
        const QString d = user.left(bs).trimmed();
        user = user.mid(bs + 1).trimmed();
        domain = (d.isEmpty() || d == QLatin1String(".")) ? QString() : d;
    }
}

// ── 拷贝 GDI 主缓冲为一帧并交付 UI（EndPaint / BitmapUpdate 共用）────────
// 节流：位图更新可能高频到达（每秒几十个小矩形），每次都全帧 memcpy +
// 触发整窗重绘会造成明显卡顿。限制交付频率 ~30fps；窗口内的更新合并
// （GDI 主缓冲已是最新合成结果，延迟拷贝不丢画面），主循环兜底冲刷。
static constexpr int kFrameIntervalMs = 33;

static void copyPrimaryFrame(rdpContext* context, const char* source) {
    auto* c = asClientContext(context);
    rdpGdi* gdi = context->gdi;
    if (!gdi || !gdi->primary_buffer || gdi->width <= 0 || gdi->height <= 0)
        return;
    if (gdi->dstFormat != PIXEL_FORMAT_BGRX32)
        return;                            // 仅支持 gdi_init 指定的 BGRX32
    const auto now = std::chrono::steady_clock::now();
    if (c->firstFrameSeen &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - c->lastFrameCopy).count()
            < kFrameIntervalMs) {
        c->frameDirty = true;              // 节流窗口内：标记脏，稍后统一交付
        return;
    }
    c->lastFrameCopy = now;
    c->frameDirty = false;
    const int w = gdi->width;
    const int h = gdi->height;
    QImage frame(w, h, QImage::Format_RGB32);   // 内存布局 B,G,R,X 与 BGRX32 一致
    const qsizetype rowBytes = qsizetype(w) * 4;
    for (int y = 0; y < h; ++y)
        std::memcpy(frame.scanLine(y), gdi->primary_buffer + qsizetype(y) * gdi->stride,
                    size_t(rowBytes));
    if (!c->firstFrameSeen) {
        c->firstFrameSeen = true;
        rdpLogInfo(QStringLiteral("收到首帧 %1x%2（来源 %3）").arg(w).arg(h).arg(source));
    }
    if (c->render)
        c->render->deliverFrame(frame);
}

// 主循环兜底：节流窗口结束后若仍有未交付的脏帧（之后不再有更新到达），
// 拷贝交付一次，保证最终画面状态不滞留
static void flushDirtyFrame(freerdp* instance) {
    auto* c = asClientContext(instance->context);
    if (!c || !c->frameDirty || !instance->context->gdi)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - c->lastFrameCopy).count()
        < kFrameIntervalMs)
        return;
    copyPrimaryFrame(instance->context, "flush");
}

// ── EndPaint：orders 绘制批次提交，拷贝 GDI 主缓冲交回 UI 线程渲染 ────────
// 注意：EndPaint 只在慢路径 orders 批次（BeginPaint/EndPaint 包裹）触发；
// 登录桌面画面主要走 fastpath 位图更新（BitmapUpdate 回调），两者都要覆盖。
static BOOL br_end_paint(rdpContext* context) {
    auto* c = asClientContext(context);
    if (c->origEndPaint && !c->origEndPaint(context))
        return FALSE;                      // 先让 GDI 完成提交，再拷帧
    copyPrimaryFrame(context, "end_paint");
    return TRUE;
}

// ── BitmapUpdate：fastpath/slowpath 位图更新（桌面画面的主要来源）────────
static BOOL br_bitmap_update(rdpContext* context, const BITMAP_UPDATE* bitmapUpdate) {
    auto* c = asClientContext(context);
    if (c->origBitmapUpdate && !c->origBitmapUpdate(context, bitmapUpdate))
        return FALSE;                      // 先让 GDI 画进 primary_buffer，再拷帧
    copyPrimaryFrame(context, "bitmap_update");
    return TRUE;
}

// ── 远端指针（光标形状）同步 ────────────────────────────────────────────
// gdi_init 注册的 graphics 不含指针原型（Pointer_Prototype == NULL），服务器
// 指针更新在缓存层被直接丢弃 → 悬停输入框/链接/边框时本地光标不跟随远端
// （无 I 型/手型/拖拽箭头），观感与 mstsc 不同步。
// 注册原型后由指针缓存自动桥接 PointerNew/PointerCached/PointerSystem。
struct BrPointer {
    rdpPointer base;              // 必须首部：Pointer_Alloc 按 size 分配并拷贝 base
    QImage* image;                // 解码后的光标像素；calloc 分配不调构造函数，只能用指针成员
};

// 解码 XOR/AND 掩码为 ARGB32 图像（仅 RDP 线程调用）
static BOOL br_pointer_new(rdpContext* context, rdpPointer* pointer) {
    auto* p = reinterpret_cast<BrPointer*>(pointer);
    if (pointer->width == 0 || pointer->height == 0 ||
        pointer->width > 256 || pointer->height > 256)
        return FALSE;                        // 防御异常尺寸
    QImage img(int(pointer->width), int(pointer->height), QImage::Format_ARGB32);
    if (img.isNull())
        return FALSE;
    const gdiPalette* pal = context->gdi ? &context->gdi->palette : nullptr;
    // PIXEL_FORMAT_BGRA32 内存字节序 B,G,R,A，与 Qt Format_ARGB32（小端）一致
    if (!freerdp_image_copy_from_pointer_data(
            img.bits(), PIXEL_FORMAT_BGRA32, 0, 0, 0, pointer->width, pointer->height,
            pointer->xorMaskData, pointer->lengthXorMask,
            pointer->andMaskData, pointer->lengthAndMask, pointer->xorBpp, pal))
        return FALSE;
    // 个别老式 32bpp 光标 XOR alpha 全 0、透明度在 AND 掩码里：
    // 不补齐则光标整体透明不可见 → 用 AND 掩码重建 alpha（1=透明，0=不透明）
    if (pointer->xorBpp == 32 && pointer->andMaskData) {
        const int w = img.width(), h = img.height();
        const auto* px = reinterpret_cast<const QRgb*>(img.constBits());
        bool anyAlpha = false;
        for (int i = 0; i < w * h && !anyAlpha; ++i)
            anyAlpha = (px[i] >> 24) != 0;
        const int andStep = (((w + 7) / 8) + 1) & ~1;   // 1bpp 行，2 字节对齐，自下而上
        if (!anyAlpha && size_t(andStep) * size_t(h) <= pointer->lengthAndMask) {
            for (int y = 0; y < h; ++y) {
                const BYTE* row = pointer->andMaskData + size_t(andStep) * (h - 1 - y);
                auto* out = reinterpret_cast<QRgb*>(img.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    const bool transparent = (row[x >> 3] & (0x80 >> (x & 7))) != 0;
                    out[x] = transparent ? QRgb(0) : (out[x] | 0xFF000000u);
                }
            }
        }
    }
    delete p->image;
    p->image = new QImage(std::move(img));
    return TRUE;
}

static void br_pointer_free(rdpContext*, rdpPointer* pointer) {
    auto* p = reinterpret_cast<BrPointer*>(pointer);
    delete p->image;
    p->image = nullptr;
}

// 应用光标到帧表面（RDP 线程 → UI 线程）；img 为空表示恢复系统默认箭头。
// 光标像素按远端 1:1 显示：会话未发 DesktopScaleFactor（远端 100% DPI），
// 与帧画面的物理像素 1:1 呈现保持一致。
static void br_apply_cursor(rdpContext* context, const QImage& img, int hotX, int hotY) {
    auto* c = asClientContext(context);
    if (!c->render || !c->render->surface())
        return;
    QWidget* surf = c->render->surface();
    QMetaObject::invokeMethod(surf, [surf, img, hotX, hotY] {
        if (img.isNull())
            surf->unsetCursor();
        else
            surf->setCursor(QCursor(QPixmap::fromImage(img), hotX, hotY));
    }, Qt::QueuedConnection);
}

static BOOL br_pointer_set(rdpContext* context, rdpPointer* pointer) {
    const auto* p = reinterpret_cast<const BrPointer*>(pointer);
    if (!p->image)
        return FALSE;
    br_apply_cursor(context, *p->image, int(pointer->xPos), int(pointer->yPos));
    return TRUE;
}

static BOOL br_pointer_set_null(rdpContext* context) {
    // SYSPTR_NULL：远端要求隐藏指针（如密码输入框），本地同步隐藏
    auto* c = asClientContext(context);
    if (!c->render || !c->render->surface())
        return FALSE;
    QWidget* surf = c->render->surface();
    QMetaObject::invokeMethod(surf, [surf] {
        surf->setCursor(Qt::BlankCursor);
    }, Qt::QueuedConnection);
    return TRUE;
}

static BOOL br_pointer_set_default(rdpContext* context) {
    br_apply_cursor(context, QImage(), 0, 0);   // SYSPTR_DEFAULT：恢复系统箭头
    return TRUE;
}

static BOOL br_pointer_set_position(rdpContext*, UINT32, UINT32) {
    return TRUE;   // 不反控本地 OS 光标位置（避免与本地鼠标拉扯）
}

// ── PreConnect：把会话参数写入 rdpSettings ──────────────────────────────
static BOOL br_pre_connect(freerdp* instance) {
    auto* c = asClientContext(instance->context);
    rdpSettings* s = instance->context->settings;
    const auto& p = c->params;

    // 凭据格式兼容：mstsc 风格 "域\用户" / ".\用户" 自动拆分（见 splitUserDomain）
    QString user = p.username;
    QString domain = p.domain;
    splitUserDomain(user, domain);
    rdpLogInfo(QStringLiteral("开始协商: %1:%2 user=%3 domain=%4 %5x%6 pwdLen=%7")
                   .arg(p.host).arg(p.port)
                   .arg(user, domain.isEmpty() ? QStringLiteral("-") : domain)
                   .arg(p.width).arg(p.height)
                   .arg(p.password.size()));
    BOOL ok = TRUE;
    ok &= freerdp_settings_set_string(s, FreeRDP_ServerHostname, p.host.toUtf8().constData());
    ok &= freerdp_settings_set_uint32(s, FreeRDP_ServerPort, UINT32(p.port));
    ok &= freerdp_settings_set_string(s, FreeRDP_Username, user.toUtf8().constData());
    if (!p.password.isEmpty())
        ok &= freerdp_settings_set_string(s, FreeRDP_Password, p.password.toUtf8().constData());
    // 密码为空时不设置（保持 NULL）：FreeRDP 的 NLA 判定 Password==NULL 才触发
    // Authenticate 弹窗；若设置成空串会静默用空密码认证 → Logon failed 且无提示
    if (!domain.isEmpty())
        ok &= freerdp_settings_set_string(s, FreeRDP_Domain, domain.toUtf8().constData());
    ok &= freerdp_settings_set_uint32(s, FreeRDP_DesktopWidth, UINT32(p.width));
    ok &= freerdp_settings_set_uint32(s, FreeRDP_DesktopHeight, UINT32(p.height));
    ok &= freerdp_settings_set_uint32(s, FreeRDP_ColorDepth, 32);
    // activation 等待超时（rdp_client_wait_for_activation 用 TcpAckTimeout 计时）。
    // 默认 9s：NLA 通过后服务器可能因登录配置文件/组策略/会话准备较慢，
    // 迟迟不发 DEMAND_ACTIVE → ERRCONNECT_ACTIVATION_TIMEOUT。mstsc 此时会一直
    // 等待（显示"请稍候"），这里对齐放宽到 60s；正常服务器秒级激活不受影响。
    ok &= freerdp_settings_set_uint32(s, FreeRDP_TcpAckTimeout, 60000);
    // ── 共享文件夹：本地驱动器重定向到远端（\\tsclient\<盘符>）─────────────
    // 逐盘符显式添加 drive 设备（Name=盘符字母），远端共享名即 \\tsclient\<盘符>，
    // 与 mstsc 一致。不能用 DrivesToRedirect="*"：其设备名为 "hotplug-all"，
    // 远端共享名会变成 \\tsclient\hotplug-all_<盘符>，路径对不上。
    // RedirectDrives 须保持 FALSE：load_addins 见 TRUE 会自动追加 "*" 设备。
    // （路径可在会话信息栏「共享文件夹」中修改，作用于下次连接）
    // 默认打开本机系统盘对应共享根，盘符动态获取（QDir::rootPath() → "C:/"）
    const QString defaultShellDir = QStringLiteral("\\\\tsclient\\\\%1")
                                        .arg(QDir::rootPath().left(1).toUpper());
    const QFileInfoList sysDrives = QDir::drives();
    for (const QFileInfo& fi : sysDrives) {
        const QString root = QDir::toNativeSeparators(fi.absoluteFilePath()); // "D:\"
        const UINT type = UINT(GetDriveTypeW(reinterpret_cast<const wchar_t*>(
            root.utf16())));   // 网络映射盘会递归重定向、光驱无意义，跳过
        if (type == DRIVE_REMOTE || type == DRIVE_CDROM)
            continue;
        const QByteArray letter = fi.absoluteFilePath().left(1).toUpper().toUtf8(); // "D"
        const QByteArray rootUtf8 = root.toUtf8();
        const char* const params[] = { "drive", letter.constData(), rootUtf8.constData() };
        if (!freerdp_client_add_device_channel(s, 3, params))
            rdpLogError(QStringLiteral("添加驱动器重定向失败: %1").arg(QString::fromUtf8(letter)));
    }
    ok &= freerdp_settings_set_bool(s, FreeRDP_RedirectDrives, FALSE);
    ok &= freerdp_settings_set_string(s, FreeRDP_ShellWorkingDirectory,
                                      p.shellDir.isEmpty() ? defaultShellDir.toUtf8().constData()
                                                           : p.shellDir.toUtf8().constData());
    // ── 剪贴板重定向：本地↔远端直接复制粘贴文本与文件 ─────────────────────
    ok &= freerdp_settings_set_bool(s, FreeRDP_RedirectClipboard, TRUE);
    // ── 网络波动自动重连（协议层；应用层 rdpLoop 另有重连循环兜底）─────────
    ok &= freerdp_settings_set_bool(s, FreeRDP_AutoReconnectionEnabled, TRUE);
    ok &= freerdp_settings_set_uint32(s, FreeRDP_AutoReconnectMaxRetries, 20);
    ok &= freerdp_settings_set_bool(s, FreeRDP_AutoLogonEnabled,
                                    p.password.isEmpty() ? FALSE : TRUE); // 有凭据才自动登录
    ok &= freerdp_settings_set_bool(s, FreeRDP_IgnoreCertificate, TRUE);  // 自签名证书直接接受
    ok &= freerdp_settings_set_bool(s, FreeRDP_NegotiateSecurityLayer, TRUE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_MouseMotion, TRUE);        // 无按键也发送移动
    // Unicode 键盘输入：连接中「共享文件夹」改动需在远端打开路径（Win+R 注入
    // 任意文本），scancode 映射受远端键盘布局影响不可靠，改用 Unicode 事件
    ok &= freerdp_settings_set_bool(s, FreeRDP_UnicodeInput, TRUE);
    ok &= freerdp_settings_set_bool(s, FreeRDP_BitmapCacheEnabled, TRUE);
    ok &= freerdp_settings_set_uint32(s, FreeRDP_KeyboardLayout, KBD_US); // en-US 基准
    if (!p.gateway.isEmpty()) {
        ok &= freerdp_settings_set_string(s, FreeRDP_GatewayHostname, p.gateway.toUtf8().constData());
        ok &= freerdp_settings_set_bool(s, FreeRDP_GatewayEnabled, TRUE);
        ok &= freerdp_settings_set_bool(s, FreeRDP_GatewayUseSameCredentials, TRUE);
    }
    if (!ok)
        return FALSE;

    // ── 通道装载：磁盘重定向/剪贴板在此真正生效 ─────────────────────────
    // RedirectDrives/DrivesToRedirect 只是开关；设备与静态通道（rdpdr、drive、
    // cliprdr、rdpsnd、drdynvc）由 freerdp_client_load_addins 按 settings 装载。
    // 官方客户端在 freerdp_client_context_new 里注册静态 addin 提供器，我们直接
    // 用 freerdp_new，需自行注册一次（全局，重复调用仅覆盖，安全）。
    if (freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0) !=
        CHANNEL_RC_OK) {
        rdpLogError(QStringLiteral("注册通道 addin 提供器失败"));
        return FALSE;
    }
    if (!freerdp_client_load_addins(instance->context->channels, s)) {
        rdpLogError(QStringLiteral("通道装载失败（rdpdr/drive/cliprdr）"));
        return FALSE;
    }
    rdpLogInfo(QStringLiteral("通道装载完成: rdpdr/drive/cliprdr/rdpsnd/drdynvc"));
    return TRUE;
}

// ── Authenticate：NLA 需要凭据或认证失败时回调，弹窗重新输入 ──────────────
// FreeRDP 在 CredSSP 认证失败（Logon failed）后会再次调用此回调重试，
// 对齐 mstsc「凭据错误当场重输」的体验。RDP 线程 → 主线程同步弹窗。
// 出参直接是 settings 字段地址（FreeRDP 约定），赋值后自动生效。
static BOOL br_authenticate(freerdp* instance, char** username, char** password, char** domain) {
    auto* c = asClientContext(instance->context);
    if (!c || !c->render)
        return FALSE;
    rdpLogInfo(QStringLiteral("Authenticate 回调触发（FreeRDP 请求凭据）"));
    rdpSettings* s = instance->context->settings;

    // 预填当前凭据（域前缀并回用户名，便于直接修改）
    const char* su = freerdp_settings_get_string(s, FreeRDP_Username);
    const char* sd = freerdp_settings_get_string(s, FreeRDP_Domain);
    QString user = QString::fromUtf8(su ? su : "");
    if (sd && *sd)
        user = QStringLiteral("%1\\%2").arg(QString::fromUtf8(sd), user);

    QString pass;
    BOOL accepted = FALSE;
    QMetaObject::invokeMethod(c->render, [&] {
        QWidget* parent = QApplication::activeWindow();
        bool ok = false;
        const QString u = QInputDialog::getText(parent, QStringLiteral("RDP 登录"),
            QStringLiteral("用户名（支持 域\\用户 / .\\用户 / user@upn）:"),
            QLineEdit::Normal, user, &ok);
        if (!ok)
            return;
        const QString pw = QInputDialog::getText(parent, QStringLiteral("RDP 登录"),
            QStringLiteral("密码:"), QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;
        user = u.trimmed();
        pass = pw;
        accepted = TRUE;
    }, Qt::BlockingQueuedConnection);

    if (!accepted)
        return FALSE;                  // 用户取消 → 终止连接

    QString dom;
    splitUserDomain(user, dom);
    rdpLogInfo(QStringLiteral("NLA 重新认证: user=%1 domain=%2")
                   .arg(user, dom.isEmpty() ? QStringLiteral("-") : dom));
    *username = _strdup(user.toUtf8().constData());
    *password = _strdup(pass.toUtf8().constData());
    *domain = dom.isEmpty() ? nullptr : _strdup(dom.toUtf8().constData());
    return TRUE;
}

// ── PostConnect：初始化 GDI 软渲染并接管 EndPaint / BitmapUpdate ─────────
static BOOL br_post_connect(freerdp* instance) {
    if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
        return FALSE;
    auto* c = asClientContext(instance->context);
    rdpUpdate* update = instance->context->update;
    c->origEndPaint = update->EndPaint;    // 保存 gdi 注册的默认实现
    update->EndPaint = br_end_paint;       // 覆写为「先提交后拷帧」
    c->origBitmapUpdate = update->BitmapUpdate;
    update->BitmapUpdate = br_bitmap_update;   // fastpath 位图更新（首帧主要来源）
    // 指针原型：接管服务器光标形状（悬停输入框→I 型、链接→手型等）。
    // gdi 默认不注册指针原型，注册前指针更新全部被缓存层丢弃。
    static const rdpPointer kPointerPrototype = {
        sizeof(BrPointer), br_pointer_new,      br_pointer_free, br_pointer_set,
        br_pointer_set_null, br_pointer_set_default, br_pointer_set_position
    };
    graphics_register_pointer(instance->context->graphics, &kPointerPrototype);
    rdpLogInfo(QStringLiteral("GDI 就绪 %1x%2，帧回调与指针同步已接管")
                   .arg(instance->context->gdi->width).arg(instance->context->gdi->height));
    return TRUE;
}

// ── PostDisconnect：释放 GDI 资源 ────────────────────────────────────────
static void br_post_disconnect(freerdp* instance) {
    gdi_free(instance);
    // 断开时卸载剪贴板后端（重连成功后 ChannelConnected 事件重新挂载）
    if (auto* c = asClientContext(instance->context))
        br_cliprdr_teardown(c);
}

// ── Qt::Key → RDP scancode（XT 扫描码；扩展键自带 KBDEXT 位，
//     freerdp_input_send_keyboard_event_ex 内部会转成 KBD_FLAGS_EXTENDED）──
static UINT32 qtKeyToRdpScancode(int key) {
    switch (key) {
        // 字母
        case Qt::Key_A: return RDP_SCANCODE_KEY_A;
        case Qt::Key_B: return RDP_SCANCODE_KEY_B;
        case Qt::Key_C: return RDP_SCANCODE_KEY_C;
        case Qt::Key_D: return RDP_SCANCODE_KEY_D;
        case Qt::Key_E: return RDP_SCANCODE_KEY_E;
        case Qt::Key_F: return RDP_SCANCODE_KEY_F;
        case Qt::Key_G: return RDP_SCANCODE_KEY_G;
        case Qt::Key_H: return RDP_SCANCODE_KEY_H;
        case Qt::Key_I: return RDP_SCANCODE_KEY_I;
        case Qt::Key_J: return RDP_SCANCODE_KEY_J;
        case Qt::Key_K: return RDP_SCANCODE_KEY_K;
        case Qt::Key_L: return RDP_SCANCODE_KEY_L;
        case Qt::Key_M: return RDP_SCANCODE_KEY_M;
        case Qt::Key_N: return RDP_SCANCODE_KEY_N;
        case Qt::Key_O: return RDP_SCANCODE_KEY_O;
        case Qt::Key_P: return RDP_SCANCODE_KEY_P;
        case Qt::Key_Q: return RDP_SCANCODE_KEY_Q;
        case Qt::Key_R: return RDP_SCANCODE_KEY_R;
        case Qt::Key_S: return RDP_SCANCODE_KEY_S;
        case Qt::Key_T: return RDP_SCANCODE_KEY_T;
        case Qt::Key_U: return RDP_SCANCODE_KEY_U;
        case Qt::Key_V: return RDP_SCANCODE_KEY_V;
        case Qt::Key_W: return RDP_SCANCODE_KEY_W;
        case Qt::Key_X: return RDP_SCANCODE_KEY_X;
        case Qt::Key_Y: return RDP_SCANCODE_KEY_Y;
        case Qt::Key_Z: return RDP_SCANCODE_KEY_Z;
        // 数字
        case Qt::Key_0: return RDP_SCANCODE_KEY_0;
        case Qt::Key_1: return RDP_SCANCODE_KEY_1;
        case Qt::Key_2: return RDP_SCANCODE_KEY_2;
        case Qt::Key_3: return RDP_SCANCODE_KEY_3;
        case Qt::Key_4: return RDP_SCANCODE_KEY_4;
        case Qt::Key_5: return RDP_SCANCODE_KEY_5;
        case Qt::Key_6: return RDP_SCANCODE_KEY_6;
        case Qt::Key_7: return RDP_SCANCODE_KEY_7;
        case Qt::Key_8: return RDP_SCANCODE_KEY_8;
        case Qt::Key_9: return RDP_SCANCODE_KEY_9;
        // F1-F12
        case Qt::Key_F1:  return RDP_SCANCODE_F1;
        case Qt::Key_F2:  return RDP_SCANCODE_F2;
        case Qt::Key_F3:  return RDP_SCANCODE_F3;
        case Qt::Key_F4:  return RDP_SCANCODE_F4;
        case Qt::Key_F5:  return RDP_SCANCODE_F5;
        case Qt::Key_F6:  return RDP_SCANCODE_F6;
        case Qt::Key_F7:  return RDP_SCANCODE_F7;
        case Qt::Key_F8:  return RDP_SCANCODE_F8;
        case Qt::Key_F9:  return RDP_SCANCODE_F9;
        case Qt::Key_F10: return RDP_SCANCODE_F10;
        case Qt::Key_F11: return RDP_SCANCODE_F11;
        case Qt::Key_F12: return RDP_SCANCODE_F12;
        // 光标 / 导航（扩展键）
        case Qt::Key_Left:     return RDP_SCANCODE_LEFT;
        case Qt::Key_Up:       return RDP_SCANCODE_UP;
        case Qt::Key_Right:    return RDP_SCANCODE_RIGHT;
        case Qt::Key_Down:     return RDP_SCANCODE_DOWN;
        case Qt::Key_Home:     return RDP_SCANCODE_HOME;
        case Qt::Key_End:      return RDP_SCANCODE_END;
        case Qt::Key_PageUp:   return RDP_SCANCODE_PRIOR;
        case Qt::Key_PageDown: return RDP_SCANCODE_NEXT;
        case Qt::Key_Insert:   return RDP_SCANCODE_INSERT;
        case Qt::Key_Delete:   return RDP_SCANCODE_DELETE;
        // 控制键
        case Qt::Key_Return:      return RDP_SCANCODE_RETURN;
        case Qt::Key_Enter:       return RDP_SCANCODE_RETURN_KP;   // 小键盘回车
        case Qt::Key_Backspace:   return RDP_SCANCODE_BACKSPACE;
        case Qt::Key_Tab:         return RDP_SCANCODE_TAB;
        case Qt::Key_Backtab:     return RDP_SCANCODE_TAB;
        case Qt::Key_Escape:      return RDP_SCANCODE_ESCAPE;
        case Qt::Key_Space:       return RDP_SCANCODE_SPACE;
        case Qt::Key_CapsLock:    return RDP_SCANCODE_CAPSLOCK;
        case Qt::Key_NumLock:     return RDP_SCANCODE_NUMLOCK;
        case Qt::Key_ScrollLock:  return RDP_SCANCODE_SCROLLLOCK;
        case Qt::Key_Print:       return RDP_SCANCODE_PRINTSCREEN;
        case Qt::Key_Pause:       return RDP_SCANCODE_PAUSE;
        case Qt::Key_Menu:        return RDP_SCANCODE_APPS;
        // 修饰键
        case Qt::Key_Shift:   return RDP_SCANCODE_LSHIFT;
        case Qt::Key_Control: return RDP_SCANCODE_LCONTROL;
        case Qt::Key_Alt:     return RDP_SCANCODE_LMENU;
        case Qt::Key_AltGr:   return RDP_SCANCODE_RMENU;
        case Qt::Key_Meta:    return RDP_SCANCODE_LWIN;   // Windows 键
        // US 布局标点
        case Qt::Key_Minus:       return RDP_SCANCODE_OEM_MINUS;  // -
        case Qt::Key_Equal:       return RDP_SCANCODE_OEM_PLUS;   // =
        case Qt::Key_BracketLeft: return RDP_SCANCODE_OEM_4;      // [
        case Qt::Key_BracketRight:return RDP_SCANCODE_OEM_6;      // ]
        case Qt::Key_Backslash:   return RDP_SCANCODE_OEM_5;      // \
        case Qt::Key_Semicolon:   return RDP_SCANCODE_OEM_1;      // ;
        case Qt::Key_Apostrophe:  return RDP_SCANCODE_OEM_7;      // '
        case Qt::Key_QuoteLeft:   return RDP_SCANCODE_OEM_3;      // `
        case Qt::Key_Comma:       return RDP_SCANCODE_OEM_COMMA;  // ,
        case Qt::Key_Period:      return RDP_SCANCODE_OEM_PERIOD; // .
        case Qt::Key_Slash:       return RDP_SCANCODE_OEM_2;      // /
        default: break;
    }
    // 小键盘数字与符号（Qt 无独立 Key 值，通过 KeypadModifier 区分，见 sendKey）
    return RDP_SCANCODE_UNKNOWN;
}

// 小键盘键（需要结合 KeypadModifier 判断；Qt 无独立的小键盘 - / . 键值，
// 统一为 Key_Minus/Key_Slash/Key_Period + KeypadModifier）
static UINT32 qtKeypadScancode(int key) {
    switch (key) {
        case Qt::Key_0: return RDP_SCANCODE_NUMPAD0;
        case Qt::Key_1: return RDP_SCANCODE_NUMPAD1;
        case Qt::Key_2: return RDP_SCANCODE_NUMPAD2;
        case Qt::Key_3: return RDP_SCANCODE_NUMPAD3;
        case Qt::Key_4: return RDP_SCANCODE_NUMPAD4;
        case Qt::Key_5: return RDP_SCANCODE_NUMPAD5;
        case Qt::Key_6: return RDP_SCANCODE_NUMPAD6;
        case Qt::Key_7: return RDP_SCANCODE_NUMPAD7;
        case Qt::Key_8: return RDP_SCANCODE_NUMPAD8;
        case Qt::Key_9: return RDP_SCANCODE_NUMPAD9;
        case Qt::Key_Asterisk: return RDP_SCANCODE_MULTIPLY;
        case Qt::Key_Plus:     return RDP_SCANCODE_ADD;
        case Qt::Key_Minus:    return RDP_SCANCODE_SUBTRACT;
        case Qt::Key_Period:   return RDP_SCANCODE_DECIMAL;
        case Qt::Key_Slash:    return RDP_SCANCODE_DIVIDE;
        case Qt::Key_Enter:    return RDP_SCANCODE_RETURN_KP;
        default: return RDP_SCANCODE_UNKNOWN;
    }
}

// Windows 平台 XT scancode → RDP scancode 直转。
// RDP 协议的键盘 scancode 就是 PC-XT scancode：低 8 位为 make code，扩展键
// （方向键/Home/End/Ins/Del/右 Ctrl/右 Alt/Win/小键盘回车等）原本带 0xE0 前缀，
// 在 RDP 里编码为 KBDEXT 标志位。Qt 在 Windows 的 nativeScanCode() 即 XT
// scancode（扩展键以 0xE000 位表示前缀），因此无需任何查表即可 1:1 转换，
// 天然覆盖 qtKeyToRdpScancode switch 之外的全部键位（OEM 标点、PrintScreen 等）。
static UINT32 nativeToRdpScancode(const QKeyEvent* e) {
    const UINT native = UINT(e->nativeScanCode());
    if (native == 0 || native > 0xFFFF)
        return RDP_SCANCODE_UNKNOWN;               // 合成事件无 native 码
    const UINT xt = native & 0xFF;
    if (xt == 0)
        return RDP_SCANCODE_UNKNOWN;
    const bool extended = (native & 0xE000) != 0;  // 0xE0 前缀（Qt 以 0xE000 位表示）
    return MAKE_RDP_SCANCODE(UINT32(xt), extended ? TRUE : FALSE);
}

// ═══════════════════════════════════════════════════════════════════════
// 帧表面：显示最近一帧远端画面 + 鼠标/键盘事件注入
// ═══════════════════════════════════════════════════════════════════════
class FreeRDPFrameSurface : public QWidget {
public:
    explicit FreeRDPFrameSurface(FreeRDPRender* r, QWidget* parent = nullptr)
        : QWidget(parent), m_r(r) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAcceptDrops(false);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);            // 无按键也接收 mouseMove
    }

    // 低级键盘钩子转发的系统级组合键（Win 键组合、Alt+Tab 等）：
    // 这些事件被 Windows 直接消费，Qt 收不到，只能由钩子直接注入远端。
    // 同样记入 m_heldKeys，失焦时随其他按住的键一起释放
    void sendHookedKey(UINT vk, UINT scan, bool extended, bool down) {
        const UINT32 sc = MAKE_RDP_SCANCODE(UINT32(scan & 0xFF), extended ? TRUE : FALSE);
        if (down) {
            if (std::find(m_heldKeys.begin(), m_heldKeys.end(), sc) == m_heldKeys.end())
                m_heldKeys.push_back(sc);
        } else {
            auto it = std::find(m_heldKeys.begin(), m_heldKeys.end(), sc);
            if (it != m_heldKeys.end())
                m_heldKeys.erase(it);
        }
        rdpLogInfo(QStringLiteral("钩子转发: %1 vk=0x%2 scan=0x%3%4")
                       .arg(down ? QStringLiteral("down") : QStringLiteral("up"))
                       .arg(vk, 2, 16, QLatin1Char('0'))
                       .arg(scan, 2, 16, QLatin1Char('0'))
                       .arg(extended ? QStringLiteral(" ext") : QString()));
        m_r->postKey(down, false, int(sc));
    }

protected:
    void showEvent(QShowEvent*) override {
        setFocus();                        // 显示即接管键盘（Tab 切入无需再点击）
    }

    // 失焦/隐藏时释放全部按住的键，避免 Alt+Tab / 切 Tab 后
    // Ctrl/Shift/Win 等修饰键在远端一直处于按下状态
    void focusOutEvent(QFocusEvent*) override { releaseHeldKeys(); }
    void hideEvent(QHideEvent*) override { releaseHeldKeys(); }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QImage f = m_r->currentFrame();
        if (f.isNull()) {
            p.fillRect(rect(), QColor(20, 20, 26));
            const QString err = m_r->lastError();
            if (!err.isEmpty()) {
                p.setPen(QColor(0xE5, 0x73, 0x73));
                p.drawText(rect().adjusted(16, 0, -16, 0),
                           Qt::AlignCenter | Qt::TextWordWrap,
                           QStringLiteral("✖ %1\n\n（详情见 日志 页 rdp 来源）").arg(err));
            } else {
                p.setPen(Qt::gray);
                p.drawText(rect(), Qt::AlignCenter, QStringLiteral("连接中…"));
            }
            return;
        }
        const QRect dst = targetRect(f);
        p.fillRect(rect(), QColor(12, 12, 16));
        const qreal dpr = devicePixelRatioF();
        if (f.size() == QSize(qRound(dst.width() * dpr), qRound(dst.height() * dpr))) {
            // 帧为物理分辨率（逻辑目标 × DPR，连接时按此请求）：切到物理像素
            // 坐标 1:1 块传输，无重采样——高 DPI 缩放下文字/图标依然锐利
            p.save();
            p.scale(1.0 / dpr, 1.0 / dpr);
            p.drawImage(qRound(dst.x() * dpr), qRound(dst.y() * dpr), f);
            p.restore();
        } else {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(dst, f);
        }
    }

    // ── 鼠标 ──
    void mousePressEvent(QMouseEvent* e) override {
        setFocus();                        // 点击即接管键盘
        sendButton(e, true);
    }
    void mouseReleaseEvent(QMouseEvent* e) override { sendButton(e, false); }
    void mouseMoveEvent(QMouseEvent* e) override {
        const QPoint fp = mapToFrame(e->position().toPoint());
        if (fp.x() >= 0)
            m_r->postMouse(PTR_FLAGS_MOVE, fp.x(), fp.y());
    }
    void wheelEvent(QWheelEvent* e) override {
        const QPoint d = e->angleDelta();
        UINT16 flags = 0;
        int steps = 0;
        if (d.y() != 0) {
            flags = PTR_FLAGS_WHEEL;
            steps = d.y();
        } else if (d.x() != 0) {
            flags = PTR_FLAGS_HWHEEL;
            steps = d.x();
        } else {
            return;
        }
        if (steps < 0)
            flags |= PTR_FLAGS_WHEEL_NEGATIVE;
        // 旋转量写入 9 位字段（一格 = 120；负值按 9 位二补码，如 -120 → 0x188）
        flags |= UINT16(steps & WheelRotationMask);
        m_r->postMouse(flags, 0, 0);       // 滚轮不带坐标，服务器用上次移动位置
    }

    // ── 键盘 ──
    void keyPressEvent(QKeyEvent* e) override   { sendKey(e, true); }
    void keyReleaseEvent(QKeyEvent* e) override { sendKey(e, false); }

    // Tab/Backtab 默认被 QWidget 用于本地焦点切换（不进 keyPressEvent）；
    // 改为直接同步远端，保证正在操作远程时全部键盘事件进远程
    bool event(QEvent* e) override {
        const int t = e->type();
        // 焦点在远程画面时接受 ShortcutOverride：阻止本地 QShortcut/QAction
        // 抢走组合键（QWidget 默认 ignore 该事件 → Ctrl+C 等被本地快捷键系统
        // 消费，到不了远端；与 QLineEdit 拦截 Ctrl+C 同款做法）
        if (t == QEvent::ShortcutOverride) {
            e->accept();
            return true;
        }
        if (t == QEvent::KeyPress || t == QEvent::KeyRelease) {
            auto* ke = static_cast<QKeyEvent*>(e);
            if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
                sendKey(ke, t == QEvent::KeyPress);
                return true;
            }
        }
        return QWidget::event(e);
    }

private:
    void sendButton(QMouseEvent* e, bool down) {
        // 按钮事件不能带 PTR_FLAGS_MOVE 位：服务器会把带 MOVE 位的事件
        // 当作纯移动处理而忽略按钮动作（官方客户端点击时也不带该位）
        UINT16 flags = 0;
        switch (e->button()) {
            case Qt::LeftButton:   flags |= PTR_FLAGS_BUTTON1; break;
            case Qt::RightButton:  flags |= PTR_FLAGS_BUTTON2; break;
            case Qt::MiddleButton: flags |= PTR_FLAGS_BUTTON3; break;
            default: return;
        }
        if (down)
            flags |= PTR_FLAGS_DOWN;
        const QPoint fp = mapToFrame(e->position().toPoint());
        rdpLogInfo(QStringLiteral("鼠标点击: flags=0x%1 widget=(%2,%3) frame=(%4,%5) frameSize=%6x%7")
                       .arg(flags, 4, 16, QLatin1Char('0'))
                       .arg(int(e->position().x())).arg(int(e->position().y()))
                       .arg(fp.x()).arg(fp.y())
                       .arg(m_r->currentFrame().width()).arg(m_r->currentFrame().height()));
        if (fp.x() >= 0)
            m_r->postMouse(flags, fp.x(), fp.y());
    }

    void sendKey(QKeyEvent* e, bool down) {
        // 解析优先级：
        // 1) nativeScanCode 直转 —— Windows 上 RDP scancode ≡ XT scancode（扩展键
        //    0xE0 前缀 → KBDEXT 位），Qt nativeScanCode 低 8 位即 XT code、0xE000
        //    位即扩展标志。零表维护、全键位覆盖（含 OEM 标点/小键盘/扩展键）。
        //    FreeRDP 公开映射表只有 X11 keycode 空间（keyboard_x11.c），且其
        //    keycode 区间与 XT 小键盘重叠（0x47-0x53），不能直接用 XT 查表。
        // 2) KeypadModifier 表 / Qt::Key switch —— 兜底合成事件（native 为 0）
        UINT32 sc = nativeToRdpScancode(e);
        if (sc == RDP_SCANCODE_UNKNOWN) {
            if (e->modifiers() & Qt::KeypadModifier)
                sc = qtKeypadScancode(e->key());
            if (sc == RDP_SCANCODE_UNKNOWN)
                sc = qtKeyToRdpScancode(e->key());
        }
        // 诊断：组合键丢失排查。autoRepeat 不记（刷屏）；未映射键也记录，
        // 用于区分「事件未到达 surface」与「映射缺失」两种丢失
        if (!e->isAutoRepeat()) {
            const auto mods = e->modifiers();
            auto modStr = [&]() {
                QStringList m;
                if (mods & Qt::ControlModifier) m << QStringLiteral("Ctrl");
                if (mods & Qt::ShiftModifier)   m << QStringLiteral("Shift");
                if (mods & Qt::AltModifier)     m << QStringLiteral("Alt");
                if (mods & Qt::MetaModifier)    m << QStringLiteral("Win");
                if (mods & Qt::KeypadModifier)  m << QStringLiteral("Keypad");
                return m.isEmpty() ? QStringLiteral("-") : m.join(QLatin1Char('+'));
            };
            rdpLogInfo(QStringLiteral("键盘: %1 key=0x%2 native=0x%3 mods=%4 → sc=0x%5%6")
                           .arg(down ? QStringLiteral("down") : QStringLiteral("up"))
                           .arg(e->key(), 4, 16, QLatin1Char('0'))
                           .arg(UINT(e->nativeScanCode()), 4, 16, QLatin1Char('0'))
                           .arg(modStr())
                           .arg(sc, 4, 16, QLatin1Char('0'))
                           .arg(sc == RDP_SCANCODE_UNKNOWN ? QStringLiteral("（未映射，丢弃）")
                                                           : QString()));
        }
        if (sc == RDP_SCANCODE_UNKNOWN)
            return;                                    // 未映射的键（如 IME 组合键）忽略
        if (down) {
            if (!e->isAutoRepeat() &&
                std::find(m_heldKeys.begin(), m_heldKeys.end(), sc) == m_heldKeys.end())
                m_heldKeys.push_back(sc);              // 记录按住状态（供失焦释放）
        } else {
            auto it = std::find(m_heldKeys.begin(), m_heldKeys.end(), sc);
            if (it != m_heldKeys.end())
                m_heldKeys.erase(it);
        }
        m_r->postKey(down, e->isAutoRepeat(), int(sc));
    }

    void releaseHeldKeys() {
        for (UINT32 sc : m_heldKeys)
            m_r->postKey(false, false, int(sc));
        m_heldKeys.clear();
    }

    QPoint mapToFrame(const QPoint& widgetPos) const {
        const QImage f = m_r->currentFrame();
        if (f.isNull())
            return QPoint(-1, -1);
        const QRect dst = targetRect(f);
        if (dst.width() <= 0 || dst.height() <= 0)
            return QPoint(-1, -1);
        const int fx = (widgetPos.x() - dst.x()) * f.width() / dst.width();
        const int fy = (widgetPos.y() - dst.y()) * f.height() / dst.height();
        if (fx < 0 || fy < 0 || fx >= f.width() || fy >= f.height())
            return QPoint(-1, -1);
        return QPoint(fx, fy);
    }

    QRect targetRect(const QImage& f) const {
        const QString mode = m_r->frameScaleMode();
        const qreal scale = scaleFactor(mode);
        if (mode == QStringLiteral("original"))
            return QRect(0, 0, f.width(), f.height());
        if (scale > 0.0 && mode != QStringLiteral("fit")) {
            const int w = qRound(f.width() * scale);
            const int h = qRound(f.height() * scale);
            return QRect((width() - w) / 2, (height() - h) / 2, w, h);
        }
        return rect();
    }

    qreal scaleFactor(const QString& mode) const {
        bool ok = false;
        const qreal v = mode.toDouble(&ok);
        return ok ? v / 100.0 : 0.0;
    }

    FreeRDPRender* m_r;
    std::vector<UINT32> m_heldKeys;    // 已发送 down 且未 release 的 scancode
};

#endif // BR_WITH_FREERDP

// ── 在未启用 FreeRDP 时的占位表面 ───────────────────────────────────────
static QLabel* makePlaceholderSurface() {
    static const QString txt =
        "FreeRDP 内嵌渲染后端。\n本构建未启用 BR_WITH_FREERDP，未接入实际图像输出。\n"
        "请运行 scripts/setup_freerdp.ps1 构建库，并以 -DBR_WITH_FREERDP=ON 重新配置 CMake。";
    auto* lbl = new QLabel(txt);
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setWordWrap(true);
    lbl->setStyleSheet(Theme::statusWarn());
    return lbl;
}

FreeRDPRender::FreeRDPRender(QObject* parent)
    : IRDPRender(parent) {
#ifdef BR_WITH_FREERDP
    m_surface = new FreeRDPFrameSurface(this);
#else
    m_surface = makePlaceholderSurface();
#endif
}

FreeRDPRender::~FreeRDPRender() {
    stop();
}

void FreeRDPRender::start(const RDPClient::SessionParams& params) {
    m_params = params;
#ifdef BR_WITH_FREERDP
    if (m_thread.joinable())
        return;                            // 已在运行
    m_error.clear();                       // 清掉上一次会话的错误提示
    m_keepRunning = true;
    m_running = true;
    m_thread = std::thread(&FreeRDPRender::rdpLoop, this, params);
#else
    emit error(QStringLiteral("当前构建未启用 FreeRDP 内嵌后端"));
#endif
}

void FreeRDPRender::stop() {
#ifdef BR_WITH_FREERDP
    m_keepRunning = false;
    if (m_thread.joinable())
        m_thread.join();
    m_running = false;
    // 恢复默认光标：会话期间可能被远端置为自定义形状或 BlankCursor
    if (m_surface)
        m_surface->unsetCursor();
#endif
}

bool FreeRDPRender::isRunning() const { return m_running; }

QWidget* FreeRDPRender::surface() { return m_surface; }

QString FreeRDPRender::backendName() const { return backendNameStatic(); }

#ifdef BR_WITH_FREERDP
void FreeRDPRender::setFrameScaleMode(const QString& mode) {
    m_scaleMode = mode;
    requestSurfaceUpdate();
}
QString FreeRDPRender::frameScaleMode() const { return m_scaleMode; }
#else
void FreeRDPRender::setFrameScaleMode(const QString&) {}
QString FreeRDPRender::frameScaleMode() const { return QString(); }
#endif

bool FreeRDPRender::isAvailable() {
#ifdef BR_WITH_FREERDP
    ensureWlogFile();  // 任何 FreeRDP API 触发 WLog 初始化前完成配置
    return true;       // 编译期可用即视为可用；运行时 lib 装载失败可在此检测
#else
    return false;
#endif
}

QString FreeRDPRender::backendNameStatic() {
#ifdef BR_WITH_FREERDP
    return QStringLiteral("FreeRDP 内嵌");
#else
    return QString();
#endif
}

QImage FreeRDPRender::currentFrame() const {
#ifdef BR_WITH_FREERDP
    std::lock_guard<std::mutex> lk(m_frameMutex);
    return m_frame;
#else
    return QImage();
#endif
}

QString FreeRDPRender::lastError() const {
#ifdef BR_WITH_FREERDP
    return m_error;
#else
    return QString();
#endif
}

QSize FreeRDPRender::desktopSize() const {
#ifdef BR_WITH_FREERDP
    std::lock_guard<std::mutex> lk(m_frameMutex);
    return m_desktopSize;
#else
    return QSize();
#endif
}

qint64 FreeRDPRender::takeFrameBytes() {
#ifdef BR_WITH_FREERDP
    return m_frameBytes.exchange(0, std::memory_order_relaxed);
#else
    return 0;
#endif
}

// ── 输入注入：UI 线程投递，RDP 线程统一发送 ─────────────────────────────
void FreeRDPRender::postMouse(int flags, int x, int y) {
#ifdef BR_WITH_FREERDP
    std::lock_guard<std::mutex> lk(m_inputMutex);
    m_pendingInput.push_back({0, flags, (y << 16) | (x & 0xFFFF)});
#else
    Q_UNUSED(flags) Q_UNUSED(x) Q_UNUSED(y)
#endif
}

void FreeRDPRender::postKey(bool down, bool repeat, int rdpScancode) {
#ifdef BR_WITH_FREERDP
    std::lock_guard<std::mutex> lk(m_inputMutex);
    m_pendingInput.push_back({1, (down ? 1 : 0) | (repeat ? 2 : 0), rdpScancode});
#else
    Q_UNUSED(down) Q_UNUSED(repeat) Q_UNUSED(rdpScancode)
#endif
}

// 低级键盘钩子拦截的系统级组合键（Win/Alt+Tab）→ 转发给输入 surface 注入
void FreeRDPRender::postHookedKey(int vk, int scan, bool extended, bool down) {
#ifdef BR_WITH_FREERDP
    auto* surf = static_cast<FreeRDPFrameSurface*>(m_surface);
    if (surf) surf->sendHookedKey(UINT(vk), UINT(scan), extended, down);
#else
    Q_UNUSED(vk) Q_UNUSED(scan) Q_UNUSED(extended) Q_UNUSED(down)
#endif
}

// ── 在远端打开路径：Win+R → Unicode 输入路径 → 回车 ────────────────────
// 用于连接中修改共享文件夹后立即在远端打开（ShellWorkingDirectory 仅登录时生效）
void FreeRDPRender::openRemotePath(const QString& path) {
#ifdef BR_WITH_FREERDP
    if (!isRunning() || path.isEmpty()) return;
    std::lock_guard<std::mutex> lk(m_inputMutex);
    // Win+R 打开「运行」对话框（LWIN 为扩展键，scancode 自带 KBDEXT 位）
    m_pendingInput.push_back({1, 1, RDP_SCANCODE_LWIN});
    m_pendingInput.push_back({1, 1, RDP_SCANCODE_KEY_R});
    m_pendingInput.push_back({1, 0, RDP_SCANCODE_KEY_R});
    m_pendingInput.push_back({1, 0, RDP_SCANCODE_LWIN});
    m_pendingInput.push_back({3, 700, 0});            // 等待对话框弹出并获得焦点
    // Unicode 逐字符输入（xfreerdp 同款节奏：按下 → 5ms → 释放 → 5ms）
    for (const QChar ch : path) {
        if (ch.unicode() == 0) continue;
        m_pendingInput.push_back({2, 1, int(ch.unicode())});
        m_pendingInput.push_back({3, 5, 0});
        m_pendingInput.push_back({2, 0, int(ch.unicode())});
        m_pendingInput.push_back({3, 5, 0});
    }
    m_pendingInput.push_back({3, 300, 0});
    m_pendingInput.push_back({1, 1, RDP_SCANCODE_RETURN});
    m_pendingInput.push_back({1, 0, RDP_SCANCODE_RETURN});
#else
    Q_UNUSED(path)
#endif
}

#ifdef BR_WITH_FREERDP
std::vector<FreeRDPRender::InputEvent> FreeRDPRender::takeInput() {
    std::lock_guard<std::mutex> lk(m_inputMutex);
    return std::exchange(m_pendingInput, {});
}

void FreeRDPRender::deliverFrame(const QImage& frame) {
    {
        std::lock_guard<std::mutex> lk(m_frameMutex);
        m_frame = frame;
        m_desktopSize = frame.size();          // 远端桌面分辨率（信息栏显示）
    }
    m_frameBytes.fetch_add(frame.sizeInBytes(), std::memory_order_relaxed);  // 网速采样
    requestSurfaceUpdate();
}

void FreeRDPRender::requestSurfaceUpdate() {
    if (m_surface) {
        QMetaObject::invokeMethod(m_surface, "update", Qt::QueuedConnection);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// RDP 事件循环线程主体
// ═══════════════════════════════════════════════════════════════════════
void FreeRDPRender::rdpLoop(const RDPClient::SessionParams& params) {
    ensureWlogFile();
    freerdp* instance = freerdp_new();
    if (!instance) {
        QMetaObject::invokeMethod(this, [this] {
            m_running = false;
            emit error(QStringLiteral("FreeRDP instance 创建失败"));
            emit finished(-1);
        }, Qt::QueuedConnection);
        return;
    }

    instance->ContextSize = sizeof(FreeRDPClientContext);
    instance->PreConnect = br_pre_connect;
    instance->PostConnect = br_post_connect;
    instance->PostDisconnect = br_post_disconnect;
    instance->Authenticate = br_authenticate;   // NLA 失败/需要凭据时弹窗重输

    if (!freerdp_context_new(instance)) {
        freerdp_free(instance);
        QMetaObject::invokeMethod(this, [this] {
            m_running = false;
            emit error(QStringLiteral("FreeRDP context 创建失败"));
            emit finished(-1);
        }, Qt::QueuedConnection);
        return;
    }

    auto* c = asClientContext(instance->context);
    c->render = this;
    c->params = params;
    // 通道连接事件 → 挂载官方 Windows 剪贴板后端（文本/文件双向复制粘贴）
    PubSub_SubscribeChannelConnected(instance->context->pubSub, br_OnChannelConnected);

    // ── 连接 + 事件循环 + 网络波动自动重连 ──────────────────────────────
    // 首连失败：凭据类错误立即终止（重试无意义），网络类错误重试 2 次；
    // 已连接后掉线：自动重连最多 5 次，间隔 2s 起指数退避（上限 15s）。
    constexpr int kMaxDropReconnect = 5;
    constexpr int kMaxInitialRetry  = 2;
    int dropAttempts = 0;
    int initialRetry = 0;
    bool everConnected = false;

    while (m_keepRunning) {
        const bool ok = freerdp_connect(instance);
        if (!ok) {
            const UINT32 err = freerdp_get_last_error(instance->context);
            QString msg = QStringLiteral("RDP 连接失败: %1 (0x%2)")
                              .arg(QString::fromUtf8(freerdp_get_last_error_string(err)))
                              .arg(err, 8, 16, QLatin1Char('0'));
            if (err == ERRCONNECT_LOGON_FAILURE)
                msg += QStringLiteral("\n凭据被服务器拒绝：请核对密码；域账户请输入「域\\用户名」；"
                                      "空密码账户默认禁止远程登录。");
            rdpLogError(msg);
            const QString wlog = readWlogTail();
            if (!wlog.isEmpty())
                rdpLogError(QStringLiteral("FreeRDP 内部日志（尾部）:\n%1").arg(wlog));

            // 凭据错误：重试不会成功，立即终止
            const bool credError = (err == ERRCONNECT_LOGON_FAILURE);
            const bool canRetry = everConnected
                ? (dropAttempts < kMaxDropReconnect)
                : (!credError && initialRetry < kMaxInitialRetry);
            if (!m_keepRunning || credError || !canRetry) {
                br_cliprdr_teardown(c);        // 兜底：事件未触发的残留清理
                freerdp_context_free(instance);
                freerdp_free(instance);
                QMetaObject::invokeMethod(this, [this, msg, credError] {
                    m_running = false;
                    m_error = msg;
                    requestSurfaceUpdate();    // 让 surface 把错误画出来，而非停在「连接中」
                    emit error(msg);
                    if (credError)
                        emit authFailed();     // 凭据被拒 → 视图弹输入框重输
                    emit finished(-1);
                }, Qt::QueuedConnection);
                return;
            }
            if (everConnected) ++dropAttempts; else ++initialRetry;
            const int delayMs = everConnected
                ? qMin(2000 * (1 << (dropAttempts - 1)), 15000)
                : 2000;
            const int attemptNo = everConnected ? dropAttempts : initialRetry;
            rdpLogInfo(QStringLiteral("连接失败，%1ms 后重试（第 %2 次）").arg(delayMs).arg(attemptNo));
            QMetaObject::invokeMethod(this, [this, attemptNo] {
                emit reconnecting(attemptNo);
            }, Qt::QueuedConnection);
            for (int waited = 0; waited < delayMs && m_keepRunning; waited += 200)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        // 连接成功（首连或重连成功）
        everConnected = true;
        dropAttempts = 0;
        initialRetry = 0;
        rdpLogInfo(QStringLiteral("RDP 会话已建立，进入事件循环"));

        // ── 输入握手：Sync + FocusIn（mstsc/xfreerdp 连接后必发）──────────
        // 缺失该握手时，部分 Windows 服务器会忽略客户端的鼠标/键盘输入
        {
            rdpInput* input = instance->context->input;
            UINT32 syncFlags = 0;
#ifdef _WIN32
            if (GetKeyState(VK_SCROLL) & 1)  syncFlags |= KBD_SYNC_SCROLL_LOCK;
            if (GetKeyState(VK_NUMLOCK) & 1) syncFlags |= KBD_SYNC_NUM_LOCK;
            if (GetKeyState(VK_CAPITAL) & 1) syncFlags |= KBD_SYNC_CAPS_LOCK;
#endif
            freerdp_input_send_synchronize_event(input, syncFlags);
            freerdp_input_send_focus_in_event(input, UINT16(syncFlags));
            // 仿 mstsc：focus-in 后补一次指针位置，服务器才开始跟随鼠标
            freerdp_input_send_mouse_event(input, PTR_FLAGS_MOVE, 0, 0);
            rdpLogInfo(QStringLiteral("输入握手已发送: syncFlags=0x%1")
                           .arg(syncFlags, 2, 16, QLatin1Char('0')));
        }

        // 诊断计数（每次连接重置）：限频记录移动事件发送，验证悬停高亮依赖的
        // 移动事件确实持续到达服务器（前 3 次全记，之后每 5s 抽样一次）
        int moveDbgCount = 0;
        auto moveDbgLast = std::chrono::steady_clock::now();

        QMetaObject::invokeMethod(this, [this] {
            m_error.clear();
            requestSurfaceUpdate();
            emit connected();
        }, Qt::QueuedConnection);

        // 事件循环：check_fds 零超时轮询收发；每轮取走 UI 投递的输入事件发送
        while (m_keepRunning) {
            if (!freerdp_check_fds(instance))
                break;                         // 断开或协议错误

            rdpInput* input = instance->context->input;
            for (const auto& ev : takeInput()) {
                if (ev.type == 0) {
                    // 鼠标：a=flags，b=(y<<16)|x
                    const UINT16 x = UINT16(ev.b & 0xFFFF);
                    const UINT16 y = UINT16((ev.b >> 16) & 0xFFFF);
                    const BOOL okSend = freerdp_input_send_mouse_event(input, UINT16(ev.a), x, y);
                    if (ev.a & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3))
                        rdpLogInfo(QStringLiteral("发送鼠标: flags=0x%1 at (%2,%3) → %4")
                                       .arg(UINT16(ev.a), 4, 16, QLatin1Char('0'))
                                       .arg(x).arg(y)
                                       .arg(okSend ? QStringLiteral("OK")
                                                   : QStringLiteral("FAILED")));
                    if (UINT16(ev.a) == PTR_FLAGS_MOVE) {
                        // 诊断（限频）：确认移动事件持续发出且坐标合理
                        const auto nowT = std::chrono::steady_clock::now();
                        if (moveDbgCount < 3 ||
                            std::chrono::duration_cast<std::chrono::seconds>(
                                nowT - moveDbgLast).count() >= 5) {
                            ++moveDbgCount;
                            moveDbgLast = nowT;
                            rdpLogInfo(QStringLiteral("发送移动 #%1: (%2,%3) → %4")
                                           .arg(moveDbgCount).arg(x).arg(y)
                                           .arg(okSend ? QStringLiteral("OK")
                                                       : QStringLiteral("FAILED")));
                        }
                    }
                } else if (ev.type == 2) {
                    // Unicode 键盘：a=1按下/0释放，b=UTF-16 码元（官方客户端同款用法）
                    const UINT16 flags = (ev.a & 1) ? 0 : KBD_FLAGS_RELEASE;
                    freerdp_input_send_unicode_keyboard_event(input, flags, UINT16(ev.b));
                } else if (ev.type == 3) {
                    // 节拍延时（毫秒）：Win+R 弹窗等待 / 字符间节奏
                    std::this_thread::sleep_for(std::chrono::milliseconds(ev.a));
                } else {
                    // 键盘：a=down|(repeat<<1)，b=scancode（扩展键自带 KBDEXT 位）
                    const BOOL down = (ev.a & 1) ? TRUE : FALSE;
                    const BOOL repeat = (ev.a & 2) ? TRUE : FALSE;
                    freerdp_input_send_keyboard_event_ex(input, down, repeat, UINT32(ev.b));
                }
            }

            flushDirtyFrame(instance);         // 节流窗口结束后的脏帧兜底交付

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (!m_keepRunning)
            break;                             // 本地主动停止，不重连
        rdpLogInfo(QStringLiteral("事件循环退出：连接断开（网络波动），准备自动重连"));
        freerdp_disconnect(instance);          // 触发 post_disconnect 释放 GDI，重连时重建
    }

    rdpLogInfo(m_keepRunning.load()
                   ? QStringLiteral("事件循环退出：重连次数耗尽或远端拒绝")
                   : QStringLiteral("事件循环退出：本地请求停止"));
    freerdp_disconnect(instance);          // 用户停止时连接可能仍在；对已断开状态安全
    br_cliprdr_teardown(c);                // 兜底：PostDisconnect 未触发的残留清理
    freerdp_context_free(instance);
    freerdp_free(instance);

    QMetaObject::invokeMethod(this, [this] {
        m_running = false;
        emit finished(0);
    }, Qt::QueuedConnection);
}
#endif // BR_WITH_FREERDP
