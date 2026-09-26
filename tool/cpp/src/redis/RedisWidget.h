#pragma once

#include <QWidget>
#include "redis/RedisClient.h"

class QTableWidget;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QSplitter;
class QTreeWidget;
class QPushButton;

/**
 * RedisWidget: Redis GUI
 * 对应原 src/views/Redis.vue
 * 左侧键列表 + 右侧键详情/命令控制台
 */
class RedisWidget : public QWidget {
    Q_OBJECT

public:
    explicit RedisWidget(QWidget* parent = nullptr);

    // 供外部调用（Dashboard 快速连接）
    void connectTo(const QString& connectionId);

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onConnected(const QVariantMap& serverInfo);
    void onDisconnected();
    void onConnectionError(const QString& msg);
    void onKeysListed(const QList<RedisClient::KeyInfo>& keys, const QString& cursor);
    void onKeySelected(int row, int column);
    void onKeyInfoReady(const RedisClient::KeyInfo& info, const QVariant& value);
    void onKeyDeleted(const QString& key, bool success);
    void onCommandResult(const QString& command, const RedisClient::CommandResult& result);
    void onExecuteCommand();
    void onRefreshKeys();
    void onDeleteKeyClicked();
    void onSearchKeys();
    void onDbChanged(int db);

private:
    void setupUI();
    void setConnectedUI(bool connected);
    void displayValue(const RedisClient::KeyInfo& info, const QVariant& value);
    QString typeName(RedisClient::RedisType type) const;

    // 连接区
    QComboBox* m_connCombo = nullptr;
    QPushButton* m_connectBtn = nullptr;
    QPushButton* m_disconnectBtn = nullptr;
    QLabel* m_serverInfoLabel = nullptr;
    QComboBox* m_dbCombo = nullptr;

    // 键列表
    QLineEdit* m_searchEdit = nullptr;
    QTableWidget* m_keyTable = nullptr;

    // 键详情
    QLabel* m_keyDetailLabel = nullptr;
    QPlainTextEdit* m_valueView = nullptr;

    // 命令控制台
    QLineEdit* m_commandInput = nullptr;
    QPlainTextEdit* m_consoleOutput = nullptr;

    RedisClient* m_client = nullptr;
    QString m_currentConnId;
};
