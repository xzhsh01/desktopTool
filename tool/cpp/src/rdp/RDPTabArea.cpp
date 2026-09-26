#include "rdp/RDPTabArea.h"
#include "core/Logger.h"

RDPTabArea::RDPTabArea(QWidget* parent) : QWidget(parent) {}

void RDPTabArea::addSession(const RDPClient::SessionParams& params,
                             const QString& label,
                             const QString& connId,
                             bool isFirst) {
    Q_UNUSED(params);
    Q_UNUSED(isFirst);
    Logger::instance().info(
        QString("RDPTabArea::addSession placeholder: %1 (%2)").arg(label, connId),
        "rdp");
    // placeholder：原实现会用 RDPClient + 渲染组件构建 tab 子页面。
}