#include "app/AboutWidget.h"
#include "app/Theme.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QGridLayout>
#include <QPushButton>
#include <QSysInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QCoreApplication>

AboutWidget::AboutWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
}

void AboutWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    auto* header = new QLabel("关于");
    header->setStyleSheet(Theme::pageHeader());
    layout->addWidget(header);

    // 应用信息卡片
    auto* card = new QWidget;
    card->setStyleSheet(Theme::card("QWidget"));
    auto* grid = new QGridLayout(card);
    grid->setContentsMargins(20, 20, 20, 20);
    grid->setSpacing(12);

    auto makeRow = [&grid](int row, const QString& key, const QString& value) {
        auto* k = new QLabel(key);
        k->setStyleSheet(QString("color: %1; font-size: 13px; border: none;").arg(Theme::kMuted));
        auto* v = new QLabel(value);
        v->setStyleSheet(QString("color: %1; font-size: 13px; border: none;").arg(Theme::kTextBright));
        grid->addWidget(k, row, 0);
        grid->addWidget(v, row, 1);
    };

    makeRow(0, "应用名称", "bambooRat (BR)");
    makeRow(1, "版本", QCoreApplication::applicationVersion());
    makeRow(2, "Qt 版本", QT_VERSION_STR);
    makeRow(3, "架构", QSysInfo::currentCpuArchitecture());
    makeRow(4, "操作系统", QSysInfo::prettyProductName());
    makeRow(5, "构建类型",
#ifdef QT_NO_DEBUG
        "Release"
#else
        "Debug"
#endif
    );

    layout->addWidget(card);
    layout->addStretch();

    // 帮助按钮
    auto* btnLayout = new QHBoxLayout;
    auto* helpBtn = new QPushButton("帮助文档");
    connect(helpBtn, &QPushButton::clicked, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/kframe/bambooRat/wiki"));
    });
    btnLayout->addWidget(helpBtn);

    auto* licenseBtn = new QPushButton("开源许可");
    connect(licenseBtn, &QPushButton::clicked, []() {
        QDesktopServices::openUrl(QUrl("https://opensource.org/licenses/MIT"));
    });
    btnLayout->addWidget(licenseBtn);

    btnLayout->addStretch();
    layout->addLayout(btnLayout);
}
