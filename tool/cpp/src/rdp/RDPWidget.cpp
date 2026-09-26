#include "rdp/RDPWidget.h"
#include "rdp/RDPClient.h"
#include "rdp/RDPTabArea.h"
#include "connections/ConnectionManager.h"

#include <QVBoxLayout>

RDPWidget::RDPWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
}

void RDPWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ── 页面级多会话 Tab 容器 ──
    m_tabArea = new RDPTabArea;
    m_tabArea->setMinimumHeight(240);
    layout->addWidget(m_tabArea, 1);
}

void RDPWidget::startSession(const QString& connectionId) {
    auto* conn = ConnectionManager::instance().getById(connectionId);
    if (!conn) return;
    RDPClient::SessionParams p;
    p.host = conn->host;
    p.port = conn->port ? conn->port : 3389;
    p.username = conn->username;
    p.password = conn->password;
    m_tabArea->addSession(p, QStringLiteral("%1:%2").arg(p.host).arg(p.port), conn->id, true);
    ConnectionManager::instance().setActive(conn->id, true);
}
