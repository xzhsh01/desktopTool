#pragma once

#include <QWidget>

class QLabel;
class QTableWidget;
class Logger;

/**
 * LogsWidget: 日志查看器
 * 对应原 src/views/Logs.vue
 */
class LogsWidget : public QWidget {
    Q_OBJECT

public:
    explicit LogsWidget(QWidget* parent = nullptr);

private slots:
    void onLogAdded();
    void refresh();
    void clearLogs();
    void exportLogs();
    void openLogFile();

private:
    void setupUI();
    void populateTable();

    QTableWidget* m_table = nullptr;
    class QComboBox* m_levelFilter = nullptr;
    class QLineEdit* m_keywordFilter = nullptr;
    QLabel* m_countLabel = nullptr;
};
