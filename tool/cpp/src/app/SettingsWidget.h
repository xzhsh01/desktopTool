#pragma once

#include <QWidget>

class QComboBox;
class QSpinBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QGroupBox;
class QPushButton;

/**
 * SettingsWidget: 设置视图
 * 对应原 src/views/Settings.vue
 * 通用设置 + 更新设置 + 数据管理
 */
class SettingsWidget : public QWidget {
    Q_OBJECT

public:
    explicit SettingsWidget(QWidget* parent = nullptr);

private slots:
    void saveSettings();
    void resetSettings();
    void clearLogs();
    void clearConnections();
    void openDataDir();
    void onSettingsLoaded();

private:
    void setupUI();
    void loadSettingsToUI();

    // 通用
    QComboBox* m_themeCombo = nullptr;
    QComboBox* m_languageCombo = nullptr;
    QCheckBox* m_trayCheck = nullptr;
    QSpinBox* m_timeoutSpin = nullptr;

    // 更新
    QCheckBox* m_autoUpdateCheck = nullptr;
    QLineEdit* m_updateUrlEdit = nullptr;

    // 数据
    QLineEdit* m_downloadDirEdit = nullptr;
    QSpinBox* m_maxFileSizeSpin = nullptr;

    // 日志
    QComboBox* m_logLevelCombo = nullptr;
    QSpinBox* m_logRetentionSpin = nullptr;

    QLabel* m_savedLabel = nullptr;
};
