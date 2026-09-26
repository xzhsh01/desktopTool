#include "app/LogsWidget.h"
#include "app/Theme.h"
#include "core/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>

LogsWidget::LogsWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
    populateTable();

    connect(&Logger::instance(), &Logger::logAdded, this, &LogsWidget::onLogAdded);
    connect(&Logger::instance(), &Logger::logsCleared, this, &LogsWidget::refresh);
}

void LogsWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    // 页面标题 + 操作按钮
    auto* headerLayout = new QHBoxLayout;
    auto* header = new QLabel("日志");
    header->setStyleSheet(Theme::pageHeader());
    headerLayout->addWidget(header);
    headerLayout->addStretch();

    auto* clearBtn = new QPushButton("清空");
    connect(clearBtn, &QPushButton::clicked, this, &LogsWidget::clearLogs);
    headerLayout->addWidget(clearBtn);

    auto* exportBtn = new QPushButton("导出");
    connect(exportBtn, &QPushButton::clicked, this, &LogsWidget::exportLogs);
    headerLayout->addWidget(exportBtn);

    auto* openFileBtn = new QPushButton("打开日志文件");
    connect(openFileBtn, &QPushButton::clicked, this, &LogsWidget::openLogFile);
    headerLayout->addWidget(openFileBtn);

    layout->addLayout(headerLayout);

    // 过滤工具栏
    auto* filterLayout = new QHBoxLayout;
    m_levelFilter = new QComboBox;
    m_levelFilter->addItem("全部级别", -1);
    m_levelFilter->addItem("DEBUG", Logger::Debug);
    m_levelFilter->addItem("INFO", Logger::Info);
    m_levelFilter->addItem("WARN", Logger::Warn);
    m_levelFilter->addItem("ERROR", Logger::Error);
    m_levelFilter->addItem("SUCCESS", Logger::Success);
    m_levelFilter->setFixedWidth(140);
    connect(m_levelFilter, &QComboBox::currentIndexChanged, this, &LogsWidget::refresh);

    m_keywordFilter = new QLineEdit;
    m_keywordFilter->setPlaceholderText("搜索日志...");
    m_keywordFilter->setFixedWidth(220);
    connect(m_keywordFilter, &QLineEdit::textChanged, this, &LogsWidget::refresh);

    m_countLabel = new QLabel;
    m_countLabel->setStyleSheet(Theme::mutedText());

    filterLayout->addWidget(m_levelFilter);
    filterLayout->addWidget(m_keywordFilter);
    filterLayout->addStretch();
    filterLayout->addWidget(m_countLabel);
    layout->addLayout(filterLayout);

    // 日志表格
    m_table = new QTableWidget;
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({"时间", "级别", "来源", "消息"});
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    layout->addWidget(m_table, 1);
}

void LogsWidget::onLogAdded() {
    // 简单策略：每次有新日志就刷新（数据量 <=1000 时性能足够）
    populateTable();
}

void LogsWidget::refresh() {
    populateTable();
}

void LogsWidget::populateTable() {
    int levelFilter = m_levelFilter->currentData().toInt();
    QString keyword = m_keywordFilter->text();

    auto logs = Logger::instance().logs();

    m_table->setRowCount(0);
    int row = 0;
    for (const auto& entry : logs) {
        // 级别过滤
        if (levelFilter >= 0 && entry.level != levelFilter) continue;
        // 关键字过滤
        if (!keyword.isEmpty() &&
            !entry.message.contains(keyword, Qt::CaseInsensitive) &&
            !entry.source.contains(keyword, Qt::CaseInsensitive)) continue;

        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(entry.timestamp.toString("yyyy-MM-dd HH:mm:ss")));

        static const QMap<int, QString> levelStr = {
            {Logger::Debug, "DEBUG"}, {Logger::Info, "INFO"},
            {Logger::Warn, "WARN"}, {Logger::Error, "ERROR"},
            {Logger::Success, "SUCCESS"}
        };
        static const QMap<int, QString> levelColor = {
            {Logger::Debug, "#888"}, {Logger::Info, "#4fc3f7"},
            {Logger::Warn, "#ffb74d"}, {Logger::Error, "#ef5350"},
            {Logger::Success, "#81c784"}
        };
        auto* levelItem = new QTableWidgetItem(levelStr.value(entry.level));
        levelItem->setForeground(QColor(levelColor.value(entry.level)));
        m_table->setItem(row, 1, levelItem);

        m_table->setItem(row, 2, new QTableWidgetItem(entry.source));
        m_table->setItem(row, 3, new QTableWidgetItem(entry.message));
        row++;
    }

    m_countLabel->setText(QString("共 %1 条日志").arg(row));
}

void LogsWidget::clearLogs() {
    auto ret = QMessageBox::question(this, "清空日志", "确定清空所有日志吗？");
    if (ret == QMessageBox::Yes) {
        Logger::instance().clear();
    }
}

void LogsWidget::exportLogs() {
    QString path = QFileDialog::getSaveFileName(this, "导出日志",
        QString("logs_%1.txt").arg(QDateTime::currentDateTime().toSecsSinceEpoch()),
        "Text File (*.txt)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "错误", "无法写入文件");
        return;
    }

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    for (const auto& entry : Logger::instance().logs()) {
        static const char* levelStr[] = {"DEBUG", "INFO", "WARN", "ERROR", "SUCCESS"};
        ts << QString("[%1] [%2] [%3] %4\n")
              .arg(entry.timestamp.toString(Qt::ISODate))
              .arg(levelStr[static_cast<int>(entry.level)])
              .arg(entry.source)
              .arg(entry.message);
    }
    QMessageBox::information(this, "成功", "日志已导出");
}

void LogsWidget::openLogFile() {
    QString path = Logger::instance().logFilePath();
    if (!path.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}
