#include "redis/RedisWidget.h"
#include "app/Theme.h"
#include "redis/RedisClient.h"
#include "connections/ConnectionManager.h"
#include "core/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTreeWidget>
#include <QPushButton>
#include <QMessageBox>
#include <QDateTime>

RedisWidget::RedisWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
    setConnectedUI(false);

    // 连接列表变化时刷新下拉框
    auto refreshCombo = [this]() {
        m_connCombo->clear();
        for (const auto& c : ConnectionManager::instance().getByType(ConnectionManager::Redis)) {
            m_connCombo->addItem(QString("%1 (%2:%3)").arg(c.name, c.host).arg(c.port), c.id);
        }
    };
    refreshCombo();
    connect(&ConnectionManager::instance(), &ConnectionManager::connectionsChanged, this, refreshCombo);
}

void RedisWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    // 标题
    auto* header = new QLabel("Redis");
    header->setStyleSheet(Theme::pageHeader());
    layout->addWidget(header);

    // 连接行
    auto* connLayout = new QHBoxLayout;
    m_connCombo = new QComboBox;
    m_connCombo->setMinimumWidth(200);
    connLayout->addWidget(m_connCombo);

    m_connectBtn = new QPushButton("连接");
    connect(m_connectBtn, &QPushButton::clicked, this, &RedisWidget::onConnectClicked);
    connLayout->addWidget(m_connectBtn);

    m_disconnectBtn = new QPushButton("断开");
    connect(m_disconnectBtn, &QPushButton::clicked, this, &RedisWidget::onDisconnectClicked);
    connLayout->addWidget(m_disconnectBtn);

    connLayout->addSpacing(16);

    // 数据库选择
    connLayout->addWidget(new QLabel("DB:"));
    m_dbCombo = new QComboBox;
    for (int i = 0; i < 16; ++i) {
        m_dbCombo->addItem(QString::number(i), i);
    }
    connect(m_dbCombo, &QComboBox::currentIndexChanged, this, &RedisWidget::onDbChanged);
    connLayout->addWidget(m_dbCombo);

    connLayout->addStretch();
    m_serverInfoLabel = new QLabel;
    m_serverInfoLabel->setStyleSheet(Theme::mutedText());
    connLayout->addWidget(m_serverInfoLabel);

    layout->addLayout(connLayout);

    // 主分割区：左侧键列表 / 右侧详情+控制台
    auto* mainSplit = new QSplitter(Qt::Horizontal);

    // ── 左侧：键列表 ──
    auto* leftPanel = new QWidget;
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    auto* searchLayout = new QHBoxLayout;
    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText("搜索键（支持 * 通配符）...");
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &RedisWidget::onSearchKeys);
    searchLayout->addWidget(m_searchEdit, 1);

    auto* refreshBtn = new QPushButton("刷新");
    connect(refreshBtn, &QPushButton::clicked, this, &RedisWidget::onRefreshKeys);
    searchLayout->addWidget(refreshBtn);

    auto* deleteKeyBtn = new QPushButton("删除键");
    connect(deleteKeyBtn, &QPushButton::clicked, this, &RedisWidget::onDeleteKeyClicked);
    searchLayout->addWidget(deleteKeyBtn);

    leftLayout->addLayout(searchLayout);

    m_keyTable = new QTableWidget;
    m_keyTable->setColumnCount(3);
    m_keyTable->setHorizontalHeaderLabels({"键", "类型", "TTL"});
    m_keyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_keyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_keyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_keyTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_keyTable->verticalHeader()->setVisible(false);
    m_keyTable->setAlternatingRowColors(true);
    connect(m_keyTable, &QTableWidget::cellClicked, this, &RedisWidget::onKeySelected);
    leftLayout->addWidget(m_keyTable, 1);

    mainSplit->addWidget(leftPanel);

    // ── 右侧：键详情 + 命令控制台 ──
    auto* rightSplit = new QSplitter(Qt::Vertical);

    // 键详情
    auto* detailPanel = new QWidget;
    auto* detailLayout = new QVBoxLayout(detailPanel);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(8);

    m_keyDetailLabel = new QLabel("键详情");
    m_keyDetailLabel->setStyleSheet(Theme::sectionHeader());
    detailLayout->addWidget(m_keyDetailLabel);

    m_valueView = new QPlainTextEdit;
    m_valueView->setReadOnly(true);
    m_valueView->setPlaceholderText("选择左侧键查看值");
    detailLayout->addWidget(m_valueView, 1);

    rightSplit->addWidget(detailPanel);

    // 命令控制台
    auto* consolePanel = new QWidget;
    auto* consoleLayout = new QVBoxLayout(consolePanel);
    consoleLayout->setContentsMargins(0, 0, 0, 0);
    consoleLayout->setSpacing(8);

    auto* consoleLabel = new QLabel("命令控制台");
    consoleLabel->setStyleSheet(Theme::sectionHeader());
    consoleLayout->addWidget(consoleLabel);

    m_consoleOutput = new QPlainTextEdit;
    m_consoleOutput->setReadOnly(true);
    m_consoleOutput->setMaximumHeight(180);
    m_consoleOutput->setPlaceholderText("命令执行结果将显示在这里");
    consoleLayout->addWidget(m_consoleOutput);

    auto* cmdLayout = new QHBoxLayout;
    m_commandInput = new QLineEdit;
    m_commandInput->setPlaceholderText("输入 Redis 命令，如: GET mykey");
    connect(m_commandInput, &QLineEdit::returnPressed, this, &RedisWidget::onExecuteCommand);
    cmdLayout->addWidget(m_commandInput, 1);

    auto* execBtn = new QPushButton("执行");
    connect(execBtn, &QPushButton::clicked, this, &RedisWidget::onExecuteCommand);
    cmdLayout->addWidget(execBtn);

    consoleLayout->addLayout(cmdLayout);
    rightSplit->addWidget(consolePanel);

    rightSplit->setStretchFactor(0, 3);
    rightSplit->setStretchFactor(1, 2);

    mainSplit->addWidget(rightSplit);
    mainSplit->setStretchFactor(0, 2);
    mainSplit->setStretchFactor(1, 3);

    layout->addWidget(mainSplit, 1);
}

void RedisWidget::setConnectedUI(bool connected) {
    m_connectBtn->setEnabled(!connected);
    m_connCombo->setEnabled(!connected);
    m_disconnectBtn->setEnabled(connected);
    m_dbCombo->setEnabled(connected);
    m_searchEdit->setEnabled(connected);
    m_keyTable->setEnabled(connected);
    if (!connected) {
        m_serverInfoLabel->setText("未连接");
        m_keyTable->setRowCount(0);
        m_valueView->clear();
        m_keyDetailLabel->setText("键详情");
    }
}

void RedisWidget::connectTo(const QString& connectionId) {
    // 选中对应项并触发连接
    int idx = m_connCombo->findData(connectionId);
    if (idx >= 0) {
        m_connCombo->setCurrentIndex(idx);
        onConnectClicked();
    }
}

void RedisWidget::onConnectClicked() {
    if (m_connCombo->count() == 0) {
        QMessageBox::information(this, "提示", "没有可用的 Redis 连接，请先在「应用管理」中创建。");
        return;
    }

    QString connId = m_connCombo->currentData().toString();
    auto* conn = ConnectionManager::instance().getById(connId);
    if (!conn) return;

    m_currentConnId = connId;

    if (m_client) {
        m_client->disconnect();
        m_client->deleteLater();
    }
    m_client = new RedisClient(this);

    connect(m_client, &RedisClient::connected, this, &RedisWidget::onConnected);
    connect(m_client, &RedisClient::disconnected, this, &RedisWidget::onDisconnected);
    connect(m_client, &RedisClient::connectionError, this, &RedisWidget::onConnectionError);
    connect(m_client, &RedisClient::keysListed, this, &RedisWidget::onKeysListed);
    connect(m_client, &RedisClient::keyInfoReady, this, &RedisWidget::onKeyInfoReady);
    connect(m_client, &RedisClient::keyDeleted, this, &RedisWidget::onKeyDeleted);
    connect(m_client, &RedisClient::commandResult, this, &RedisWidget::onCommandResult);

    RedisClient::ConnectParams params;
    params.host = conn->host;
    params.port = conn->port;
    params.password = conn->password;
    params.database = 0;

    m_serverInfoLabel->setText("连接中...");
    ConnectionManager::instance().setActive(connId, true);
    m_client->connectTo(params);
}

void RedisWidget::onDisconnectClicked() {
    if (m_client) m_client->disconnect();
}

void RedisWidget::onConnected(const QVariantMap& serverInfo) {
    setConnectedUI(true);

    QString info;
    if (serverInfo.contains("redis_version")) {
        info = QString("Redis %1").arg(serverInfo["redis_version"].toString());
    } else {
        info = "已连接";
    }
    if (serverInfo.contains("used_memory_human")) {
        info += QString("  |  内存: %1").arg(serverInfo["used_memory_human"].toString());
    }
    if (serverInfo.contains("connected_clients")) {
        info += QString("  |  客户端: %1").arg(serverInfo["connected_clients"].toString());
    }
    m_serverInfoLabel->setText(info);

    // 自动加载键列表
    onRefreshKeys();
}

void RedisWidget::onDisconnected() {
    setConnectedUI(false);
    ConnectionManager::instance().setActive(m_currentConnId, false);
}

void RedisWidget::onConnectionError(const QString& msg) {
    m_serverInfoLabel->setText("连接失败");
    ConnectionManager::instance().setActive(m_currentConnId, false);
    QMessageBox::warning(this, "Redis 连接错误", msg);
}

void RedisWidget::onRefreshKeys() {
    if (m_client && m_client->isConnected()) {
        m_client->scanKeys("*");
    }
}

void RedisWidget::onSearchKeys() {
    if (m_client && m_client->isConnected()) {
        QString pattern = m_searchEdit->text();
        if (pattern.isEmpty()) pattern = "*";
        // 确保包含通配符
        if (!pattern.contains('*')) pattern = "*" + pattern + "*";
        m_client->scanKeys(pattern);
    }
}

void RedisWidget::onKeysListed(const QList<RedisClient::KeyInfo>& keys, const QString& cursor) {
    Q_UNUSED(cursor)

    m_keyTable->setRowCount(0);
    int row = 0;
    for (const auto& info : keys) {
        m_keyTable->insertRow(row);

        auto* keyItem = new QTableWidgetItem(info.key);
        m_keyTable->setItem(row, 0, keyItem);

        m_keyTable->setItem(row, 1, new QTableWidgetItem(typeName(info.type)));

        QString ttlStr;
        if (info.ttl == -1) ttlStr = "∞";
        else if (info.ttl == -2) ttlStr = "-";
        else ttlStr = QString::number(info.ttl) + "s";
        m_keyTable->setItem(row, 2, new QTableWidgetItem(ttlStr));

        row++;
    }

    m_serverInfoLabel->setText(m_serverInfoLabel->text() + QString("  |  %1 个键").arg(keys.size()));
}

void RedisWidget::onKeySelected(int row, int) {
    auto* item = m_keyTable->item(row, 0);
    if (!item || !m_client) return;

    QString key = item->text();
    m_keyDetailLabel->setText(QString("键详情: %1").arg(key));
    m_valueView->setPlainText("加载中...");
    m_client->getKeyInfo(key);
}

void RedisWidget::onKeyInfoReady(const RedisClient::KeyInfo& info, const QVariant& value) {
    displayValue(info, value);
}

void RedisWidget::displayValue(const RedisClient::KeyInfo& info, const QVariant& value) {
    QString header = QString("键: %1\n类型: %2\nTTL: %3\n大小: %4\n\n")
        .arg(info.key)
        .arg(typeName(info.type))
        .arg(info.ttl == -1 ? "永久" : QString("%1 秒").arg(info.ttl))
        .arg(info.size);

    QString body;
    switch (info.type) {
        case RedisClient::String:
            body = value.toString();
            break;
        case RedisClient::List:
        case RedisClient::Set: {
            auto list = value.toList();
            for (int i = 0; i < list.size(); ++i) {
                body += QString("%1) %2\n").arg(i).arg(list[i].toString());
            }
            break;
        }
        case RedisClient::Hash: {
            auto hash = value.toHash();
            for (auto it = hash.begin(); it != hash.end(); ++it) {
                body += QString("%1: %2\n").arg(it.key(), it.value().toString());
            }
            break;
        }
        case RedisClient::ZSet: {
            auto list = value.toList();
            for (int i = 0; i < list.size(); ++i) {
                auto pair = list[i].toHash();
                body += QString("%1) %2 (score: %3)\n")
                    .arg(i)
                    .arg(pair["member"].toString())
                    .arg(pair["score"].toString());
            }
            break;
        }
        default:
            body = value.toString();
    }

    m_valueView->setPlainText(header + body);
}

void RedisWidget::onDeleteKeyClicked() {
    int row = m_keyTable->currentRow();
    if (row < 0) {
        QMessageBox::information(this, "提示", "请先选择要删除的键");
        return;
    }

    QString key = m_keyTable->item(row, 0)->text();
    auto ret = QMessageBox::question(this, "删除键",
        QString("确定删除键「%1」吗？").arg(key));
    if (ret == QMessageBox::Yes && m_client) {
        m_client->deleteKey(key);
    }
}

void RedisWidget::onKeyDeleted(const QString& key, bool success) {
    if (success) {
        m_consoleOutput->appendPlainText(QString("[OK] 键 %1 已删除").arg(key));
        onRefreshKeys();
    } else {
        m_consoleOutput->appendPlainText(QString("[ERR] 删除键 %1 失败").arg(key));
    }
}

void RedisWidget::onExecuteCommand() {
    QString command = m_commandInput->text().trimmed();
    if (command.isEmpty() || !m_client) return;

    m_consoleOutput->appendPlainText(QString("> %1").arg(command));
    m_client->executeCommand(command);
    m_commandInput->clear();
}

void RedisWidget::onCommandResult(const QString& command, const RedisClient::CommandResult& result) {
    QString output;
    if (!result.success) {
        output = QString("(error) %1").arg(result.error);
    } else if (result.typeString == "array") {
        auto list = result.value.toList();
        output = QString("(%1 项)").arg(list.size());
        for (int i = 0; i < list.size() && i < 100; ++i) {
            output += QString("\n%1) %2").arg(i).arg(list[i].toString());
        }
        if (list.size() > 100) output += "\n...";
    } else if (result.typeString == "nil") {
        output = "(nil)";
    } else if (result.typeString == "integer") {
        output = QString("(integer) %1").arg(result.value.toLongLong());
    } else {
        output = QString("\"%1\"").arg(result.value.toString());
    }

    m_consoleOutput->appendPlainText(output);
    m_consoleOutput->appendPlainText(QString());

    // 如果是影响键的命令，刷新键列表
    QString cmdUpper = command.toUpper();
    if (cmdUpper.startsWith("SET") || cmdUpper.startsWith("DEL") ||
        cmdUpper.startsWith("HSET") || cmdUpper.startsWith("LPUSH") ||
        cmdUpper.startsWith("EXPIRE")) {
        onRefreshKeys();
    }
}

void RedisWidget::onDbChanged(int) {
    if (m_client && m_client->isConnected()) {
        m_client->selectDatabase(m_dbCombo->currentData().toInt());
        onRefreshKeys();
    }
}

QString RedisWidget::typeName(RedisClient::RedisType type) const {
    switch (type) {
        case RedisClient::String: return "string";
        case RedisClient::List: return "list";
        case RedisClient::Hash: return "hash";
        case RedisClient::Set: return "set";
        case RedisClient::ZSet: return "zset";
        case RedisClient::Stream: return "stream";
        default: return "none";
    }
}
