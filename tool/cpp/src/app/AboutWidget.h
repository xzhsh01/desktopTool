#pragma once

#include <QWidget>

class QLabel;

/**
 * AboutWidget: 关于页面
 * 对应原 src/views/Settings.vue 中的"关于"卡片 + Help.vue
 */
class AboutWidget : public QWidget {
    Q_OBJECT

public:
    explicit AboutWidget(QWidget* parent = nullptr);

private:
    void setupUI();
};
