#include "app/SettingsWidget.h"
#include "app/Theme.h"
#include "core/Labels.h"
#include "core/Settings.h"
#include "core/Logger.h"
#include "connections/ConnectionManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QUrl>
#include <QTimer>
#include <QScrollArea>

SettingsWidget::SettingsWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
    loadSettingsToUI();
}

void SettingsWidget::setupUI() {
    // 外层：滚动区域包裹全部内容，避免窗口较小时下方控件被裁剪
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; }");

    auto* content = new QWidget;
    content->setStyleSheet("background: transparent;");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    scroll->setWidget(content);
    outerLayout->addWidget(scroll);

    // 标题
    auto* header = new QLabel(Labels::instance().get("settings.title", "设置"));
    header->setStyleSheet(Theme::pageHeader());
    layout->addWidget(header);

    // ── 通用设置 ──
    auto* generalGroup = new QGroupBox(Labels::instance().get("settings.general.title", "通用"));
    auto* generalForm = new QFormLayout(generalGroup);
    generalForm->setSpacing(10);

    m_themeCombo = new QComboBox;
    m_themeCombo->addItem(Labels::instance().get("settings.general.themeDark", "深色"), "dark");
    m_themeCombo->addItem(Labels::instance().get("settings.general.themeLight", "浅色"), "light");
    generalForm->addRow(Labels::instance().get("settings.general.theme", "主题:"), m_themeCombo);

    m_languageCombo = new QComboBox;
    m_languageCombo->addItem(Labels::instance().get("settings.general.langZh", "简体中文"), "zh-CN");
    m_languageCombo->addItem(Labels::instance().get("settings.general.langEn", "English"), "en-US");
    generalForm->addRow(Labels::instance().get("settings.general.language", "语言:"), m_languageCombo);

    m_trayCheck = new QCheckBox(Labels::instance().get("settings.general.minimizeToTray",
                                                        "关闭窗口时最小化到系统托盘"));
    generalForm->addRow("", m_trayCheck);

    m_timeoutSpin = new QSpinBox;
    m_timeoutSpin->setRange(5, 300);
    m_timeoutSpin->setSuffix(Labels::instance().get("settings.general.timeoutUnit", " 秒"));
    generalForm->addRow(Labels::instance().get("settings.general.timeout", "连接超时:"), m_timeoutSpin);

    layout->addWidget(generalGroup);

    // ── 更新设置 ──
    auto* updateGroup = new QGroupBox(Labels::instance().get("settings.update.title", "更新"));
    auto* updateForm = new QFormLayout(updateGroup);
    updateForm->setSpacing(10);

    m_autoUpdateCheck = new QCheckBox(Labels::instance().get("settings.update.autoCheck",
                                                            "启动时自动检查更新"));
    updateForm->addRow("", m_autoUpdateCheck);

    m_updateUrlEdit = new QLineEdit;
    m_updateUrlEdit->setPlaceholderText(Labels::instance().get("settings.update.urlPlaceholder",
                                                               "更新服务器 URL"));
    updateForm->addRow(Labels::instance().get("settings.update.urlLabel", "更新地址:"),
                       m_updateUrlEdit);

    layout->addWidget(updateGroup);

    // ── 数据管理 ──
    auto* dataGroup = new QGroupBox(Labels::instance().get("settings.data.title", "数据管理"));
    auto* dataForm = new QFormLayout(dataGroup);
    dataForm->setSpacing(10);

    {
        auto* wrap = new QWidget;
        auto* hl = new QHBoxLayout(wrap);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);
        m_downloadDirEdit = new QLineEdit;
        m_downloadDirEdit->setPlaceholderText(Labels::instance().get(
            "settings.data.downloadDirPlaceholder", "文件下载默认目录"));
        auto* browseBtn = new QPushButton(Labels::instance().get("common.browse", "浏览..."));
        browseBtn->setFixedWidth(70);
        connect(browseBtn, &QPushButton::clicked, this, [this]() {
            QString dir = QFileDialog::getExistingDirectory(this, "选择下载目录");
            if (!dir.isEmpty()) m_downloadDirEdit->setText(dir);
        });
        hl->addWidget(m_downloadDirEdit);
        hl->addWidget(browseBtn);
        dataForm->addRow(Labels::instance().get("settings.data.downloadDirLabel", "下载目录:"),
                         wrap);
    }

    m_maxFileSizeSpin = new QSpinBox;
    m_maxFileSizeSpin->setRange(1, 10000);
    m_maxFileSizeSpin->setSuffix(Labels::instance().get("settings.data.maxFileSizeUnit", " MB"));
    dataForm->addRow(Labels::instance().get("settings.data.maxFileSize", "最大文件大小:"),
                     m_maxFileSizeSpin);

    // 数据目录 + 数据操作
    {
        auto* wrap = new QWidget;
        auto* hl = new QHBoxLayout(wrap);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(8);

        auto* openDataBtn = new QPushButton(Labels::instance().get("settings.data.openDirBtn",
                                                                  "打开数据目录"));
        connect(openDataBtn, &QPushButton::clicked, this, &SettingsWidget::openDataDir);
        hl->addWidget(openDataBtn);

        auto* clearLogsBtn = new QPushButton(Labels::instance().get("settings.data.clearLogsBtn",
                                                                    "清空日志"));
        connect(clearLogsBtn, &QPushButton::clicked, this, &SettingsWidget::clearLogs);
        hl->addWidget(clearLogsBtn);

        auto* clearConnsBtn = new QPushButton(Labels::instance().get("settings.data.clearConnsBtn",
                                                                     "删除所有连接"));
        connect(clearConnsBtn, &QPushButton::clicked, this, &SettingsWidget::clearConnections);
        hl->addWidget(clearConnsBtn);

        hl->addStretch();
        dataForm->addRow(Labels::instance().get("settings.data.actionsLabel", "数据操作:"), wrap);
    }

    layout->addWidget(dataGroup);

    // 说明：原"设置 → 邮件加密"（S/MIME 证书管理）已移入"邮箱编辑 → 邮件加密 tab"

    // ── 日志设置 ──
    auto* logGroup = new QGroupBox(Labels::instance().get("settings.log.title", "日志"));
    auto* logForm = new QFormLayout(logGroup);
    logForm->setSpacing(10);

    m_logLevelCombo = new QComboBox;
    m_logLevelCombo->addItem(Labels::instance().get("settings.log.levelDebug", "Debug"), "debug");
    m_logLevelCombo->addItem(Labels::instance().get("settings.log.levelInfo",  "Info"),  "info");
    m_logLevelCombo->addItem(Labels::instance().get("settings.log.levelWarn",  "Warn"),  "warn");
    m_logLevelCombo->addItem(Labels::instance().get("settings.log.levelError", "Error"), "error");
    logForm->addRow(Labels::instance().get("settings.log.levelLabel", "日志级别:"), m_logLevelCombo);

    m_logRetentionSpin = new QSpinBox;
    m_logRetentionSpin->setRange(1, 365);
    m_logRetentionSpin->setSuffix(Labels::instance().get("settings.log.retentionUnit", " 天"));
    logForm->addRow(Labels::instance().get("settings.log.retentionLabel", "日志保留:"),
                    m_logRetentionSpin);

    layout->addWidget(logGroup);

    // ── 保存/重置按钮 ──
    auto* btnLayout = new QHBoxLayout;

    auto* saveBtn = new QPushButton(Labels::instance().get("settings.actions.saveBtn", "保存设置"));
    saveBtn->setMinimumHeight(34);
    connect(saveBtn, &QPushButton::clicked, this, &SettingsWidget::saveSettings);
    btnLayout->addWidget(saveBtn);

    auto* resetBtn = new QPushButton(Labels::instance().get("settings.actions.resetBtn", "恢复默认"));
    connect(resetBtn, &QPushButton::clicked, this, &SettingsWidget::resetSettings);
    btnLayout->addWidget(resetBtn);

    m_savedLabel = new QLabel;
    m_savedLabel->setStyleSheet(Theme::statusOk());
    m_savedLabel->hide();
    btnLayout->addWidget(m_savedLabel);

    btnLayout->addStretch();
    layout->addLayout(btnLayout);

    layout->addStretch();
}

void SettingsWidget::loadSettingsToUI() {
    auto& s = Settings::instance();

    m_themeCombo->setCurrentIndex(m_themeCombo->findData(s.theme()));
    m_languageCombo->setCurrentIndex(m_languageCombo->findData(s.language()));
    m_trayCheck->setChecked(s.minimizeToTray());
    m_timeoutSpin->setValue(s.connectTimeout());

    m_autoUpdateCheck->setChecked(s.autoCheckUpdate());
    m_updateUrlEdit->setText(s.updateUrl());

    m_downloadDirEdit->setText(s.downloadDir());
    m_maxFileSizeSpin->setValue(s.maxFileSize());

    m_logLevelCombo->setCurrentIndex(m_logLevelCombo->findData(s.logLevel()));
    m_logRetentionSpin->setValue(s.logRetentionDays());
}

void SettingsWidget::saveSettings() {
    QVariantMap changes;
    changes["theme"] = m_themeCombo->currentData().toString();
    changes["language"] = m_languageCombo->currentData().toString();
    changes["minimizeToTray"] = m_trayCheck->isChecked();
    changes["connectTimeout"] = m_timeoutSpin->value();

    changes["autoCheckUpdate"] = m_autoUpdateCheck->isChecked();
    changes["updateUrl"] = m_updateUrlEdit->text();

    changes["downloadDir"] = m_downloadDirEdit->text();
    changes["maxFileSize"] = m_maxFileSizeSpin->value();

    changes["logLevel"] = m_logLevelCombo->currentData().toString();
    changes["logRetentionDays"] = m_logRetentionSpin->value();

    Settings::instance().update(changes);
    Logger::instance().info(Labels::instance().get("settings.actions.savedLog", "设置已保存"),
                            "settings");

    // 显示保存成功提示
    m_savedLabel->setText(Labels::instance().get("common.savedOk", "✓ 已保存"));
    m_savedLabel->show();
    QTimer::singleShot(2000, m_savedLabel, &QLabel::hide);
}

void SettingsWidget::resetSettings() {
    auto ret = QMessageBox::question(this,
        Labels::instance().get("settings.actions.resetTitle", "恢复默认"),
        Labels::instance().get("settings.actions.resetConfirm",
                               "确定将所有设置恢复为默认值吗？"));
    if (ret != QMessageBox::Yes) return;

    // 重置为默认值
    Settings::instance().update({
        {"minimizeToTray", true},
        {"theme", "dark"},
        {"language", "zh-CN"},
        {"connectTimeout", 30},
        {"autoCheckUpdate", true},
        {"updateUrl", ""},
        {"downloadDir", ""},
        {"maxFileSize", 500},
        {"logLevel", "info"},
        {"logRetentionDays", 30}
    });

    loadSettingsToUI();
    Logger::instance().info("设置已恢复默认", "settings");
}

void SettingsWidget::clearLogs() {
    auto ret = QMessageBox::question(this, "清空日志", "确定清空所有日志吗？");
    if (ret == QMessageBox::Yes) {
        Logger::instance().clear();
    }
}

void SettingsWidget::clearConnections() {
    auto ret = QMessageBox::question(this, "删除连接",
        "确定删除所有已保存的连接吗？\n此操作不可恢复！");
    if (ret != QMessageBox::Yes) return;

    auto& cm = ConnectionManager::instance();
    const auto ids = [&]() {
        QStringList list;
        for (const auto& c : cm.connections()) list << c.id;
        return list;
    }();
    for (const QString& id : ids) {
        cm.remove(id);
    }
    Logger::instance().info("所有连接已删除", "settings");
}

void SettingsWidget::openDataDir() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void SettingsWidget::onSettingsLoaded() {
    loadSettingsToUI();
}
