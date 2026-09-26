#pragma once
#include <QString>
#include <QWidget>
#include "rdp/RDPClient.h"

/**
 * RDPTabArea — RDP 多会话标签容器（重构中 placeholder）
 *
 * 原始实现在 rdp 目录内已被清理，本文件提供最小可用桩以满足 RDPWidget
 * 集成：仅承载 tab 容器与 addSession 接入。完整功能（标签拖拽、关闭、
 * 状态指示）后续按 RDP 需求补回。
 */
class RDPTabArea : public QWidget {
    Q_OBJECT
public:
    explicit RDPTabArea(QWidget* parent = nullptr);
    void addSession(const RDPClient::SessionParams& params,
                    const QString& label,
                    const QString& connId,
                    bool isFirst);
};