#include "rdp/RDPRender.h"

#include "app/Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QDateTime>

// ── SystemRDPRender：调用系统原生 RDP 客户端 ──────────────────────────────
// 远端桌面由外部进程（mstsc / xfreerdp / rdesktop）弹出独立窗口显示，
// 应用内窗口（surface）显示连接状态卡，便于统一纳入多窗口工作区管理。
// 无独立 Q_OBJECT：仅继承并转发 IRDPRender 的信号（基类已带 Q_OBJECT）。
class SystemRDPRender final : public IRDPRender {
public:
    explicit SystemRDPRender(QObject* parent = nullptr)
        : IRDPRender(parent),
          m_client(new RDPClient(this)),
          m_surface(new QWidget) {
        connect(m_client, &RDPClient::sessionStarted, this, &SystemRDPRender::connected);
        connect(m_client, &RDPClient::sessionError,  this, &SystemRDPRender::error);
        connect(m_client, &RDPClient::sessionFinished, this, &SystemRDPRender::finished);
        buildSurface();
    }

    void start(const RDPClient::SessionParams& p) override {
        m_params = p;
        m_hostLabel->setText(QString("%1:%2").arg(p.host).arg(p.port));
        m_userLabel->setText(p.username.isEmpty() ? "（未指定）" : p.username);
        m_statusLabel->setText("启动中…");
        m_client->startSession(p);
    }
    void stop() override {
        // 外部进程由用户自行关闭；这里仅重置状态
        m_statusLabel->setText("已停止");
    }
    bool isRunning() const override {
        return false; // 外部进程独立运行，应用内无法可靠跟踪
    }
    QWidget* surface() override { return m_surface; }
    QString backendName() const override {
        return QStringLiteral("系统客户端 (%1)").arg(RDPClient::clientName());
    }

private:
    void buildSurface() {
        m_surface->setStyleSheet(
            QString("QFrame#rdpStatusCard { background: %1; border: 1px solid %2; border-radius: 6px; }")
                .arg(Theme::kSidebar, Theme::kBorder));
        auto* lay = new QVBoxLayout(m_surface);
        lay->setContentsMargins(16, 16, 16, 16);

        auto* card = new QFrame;
        card->setObjectName("rdpStatusCard");
        auto* v = new QVBoxLayout(card);
        v->setSpacing(8);

        auto* title = new QLabel("远程桌面会话");
        title->setStyleSheet(Theme::sectionHeader());
        v->addWidget(title);

        auto* info = new QLabel;
        info->setWordWrap(true);
        info->setStyleSheet(Theme::mutedText());
        info->setText(QStringLiteral("远端会话已在此后台启动，实际画面由系统 RDP 客户端窗口承载。\n"
                                     "关闭外部窗口即结束会话。若要应用内联嵌显示，请编译启用 FreeRDP 内嵌后端。"));
        v->addWidget(info);

        m_hostLabel = new QLabel;
        m_hostLabel->setStyleSheet(Theme::sectionHeader());
        v->addWidget(m_hostLabel);

        m_userLabel = new QLabel;
        m_userLabel->setStyleSheet(Theme::mutedText());
        v->addWidget(m_userLabel);

        m_statusLabel = new QLabel;
        m_statusLabel->setStyleSheet(Theme::statusOk());
        v->addWidget(m_statusLabel);

        lay->addWidget(card);
        lay->addStretch();
    }

    RDPClient::SessionParams m_params;
    QWidget* m_surface = nullptr;
    QLabel*  m_hostLabel = nullptr;
    QLabel*  m_userLabel = nullptr;
    QLabel*  m_statusLabel = nullptr;
    RDPClient* m_client = nullptr;
};

#include "rdp/RDPEmbedded.h"

IRDPRender* createRDPRender(QObject* parent) {
#ifdef BR_WITH_FREERDP
    if (FreeRDPRender::isAvailable()) {
        return new FreeRDPRender(parent);
    }
#endif
    return new SystemRDPRender(parent);
}

QString rdpRenderBackendName() {
#ifdef BR_WITH_FREERDP
    if (FreeRDPRender::isAvailable()) {
        return FreeRDPRender::backendNameStatic();
    }
#endif
    QString n = RDPClient::clientName();
    return n.isEmpty() ? QString() : QStringLiteral("系统客户端 (%1)").arg(n);
}