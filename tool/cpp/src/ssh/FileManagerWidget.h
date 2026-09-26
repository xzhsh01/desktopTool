#pragma once

#include <QWidget>

#include "ssh/SFTPClient.h"

class QTableWidget;
class QLineEdit;
class QComboBox;
class QProgressBar;
class QLabel;
class QPushButton;

/**
 * FileManagerWidget: SFTP 文件管理器
 * 对应前 src/views/FileManager.vue
 * 通过 SFTPClient（基于 libssh）+ 工作线程执行实际传输
 */
class FileManagerWidget : public QWidget {
    Q_OBJECT

public:
    explicit FileManagerWidget(QWidget* parent = nullptr);

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onSftpConnected();
    void onSftpDisconnected();
    void onSftpError(const QString& msg);
    void onDirectoryListed(const QString& path,
                            const QList<SFTPClient::FileEntry>& entries);
    void onTransferFinished(const QString& path, bool success, const QString& error);
    void onOperationFinished(const QString& op, bool success, const QString& error);
    void onItemDoubleClicked(int row, int column);
    void onRefreshClicked();
    void onUpClicked();
    void onDownloadClicked();
    void onUploadClicked();
    void onDeleteClicked();
    void onRenameClicked();
    void onMkdirClicked();

private:
    void setupUI();
    void setConnectedUI(bool connected);
    void navigateTo(const QString& path);
    QString joinPath(const QString& base, const QString& name) const;
    QString parentPath(const QString& path) const;
    QString selectedRemotePath() const;
    void showTransferBar(bool visible, const QString& label = QString());

    QComboBox* m_connCombo = nullptr;
    QPushButton* m_connectBtn = nullptr;
    QPushButton* m_disconnectBtn = nullptr;
    QLineEdit* m_pathEdit = nullptr;
    QTableWidget* m_table = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QLabel* m_progressLabel = nullptr;
    QLabel* m_statusLabel = nullptr;

    SFTPClient* m_sftp = nullptr;          // owned, set parent in connectTo
    QString m_currentPath;
    QString m_currentConnId;
};