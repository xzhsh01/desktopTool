#pragma once

#include <QWidget>

#include "rdp/RDPClient.h"

class RDPTabArea;

/**
 * RDPWidget: 远程桌面视图
 * 对应原 src/views/RDP.vue
 * 页面级多会话 Tab 容器（每个 Tab 一个会话，占满切换）；连接通过右上角「+」或外部 startSession() 发起
 */
class RDPWidget : public QWidget {
    Q_OBJECT

public:
    explicit RDPWidget(QWidget* parent = nullptr);

    // 供外部调用（Dashboard / 连接管理快速连接）
    void startSession(const QString& connectionId);

private:
    void setupUI();

    RDPTabArea* m_tabArea = nullptr;
};