#include "ssh/FileManagerWidget.h"

#include "app/Theme.h"
#include "connections/ConnectionManager.h"
#include "core/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QTimer>

// 辅助：格式化字节数
static QString formatBytes(qint64 bytes) {
    if (bytes > 1024 * 1024 * 1024) {
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
    }
    if (bytes > 1024 * 1024) {
        return QString::number(bytes / (1024.0 * 1024), 'f', 1) + " MB";
    }
    if (bytes > 1024) {
        return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    }
    return QString::number(bytes) + " B";
}

// 权限字 → 八进制字符串（八进制）
static QString permsToOctal(quint32 perm) {
    return QString("0%1").arg(perm & 0x1FF, 3, 8, QChar('0'));
}

// ════════════════════════════════════════════════════════════════════════════════
// FileManagerWidget
// ════════════════════════════════════════════════════════════════════════════════

FileManagerWidget::FileManagerWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
    setConnectedUI(false);

    // 刷新连接下拉框
    connect(&ConnectionManager::instance(), &ConnectionManager::connectionsChanged,
            this, [this]() {
        m_connCombo->clear();
        for (const auto& c : ConnectionManager::instance().getByType(ConnectionManager::SSH)) {
            m_connCombo->addItem(QString("%1 (%2:%3)").arg(c.name, c.host).arg(c.port), c.id);
        }
    });
    for (const auto& c : ConnectionManager::instance().getByType(ConnectionManager::SSH)) {
        m_connCombo->addItem(QString("%1 (%2:%3)").arg(c.name, c.host).arg(c.port), c.id);
    }
}

void FileManagerWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    auto* header = new QLabel("文件管理");
    header->setStyleSheet(Theme::pageHeader());
    layout->addWidget(header);

    // 连接栏
    auto* connLayout = new QHBoxLayout;
    m_connCombo = new QComboBox;
    m_connCombo->setMinimumWidth(220);
    connLayout->addWidget(m_connCombo);

    m_connectBtn = new QPushButton("连接");
    connect(m_connectBtn, &QPushButton::clicked, this, &FileManagerWidget::onConnectClicked);
    connLayout->addWidget(m_connectBtn);

    m_disconnectBtn = new QPushButton("断开");
    connect(m_disconnectBtn, &QPushButton::clicked, this, &FileManagerWidget::onDisconnectClicked);
    connLayout->addWidget(m_disconnectBtn);

    connLayout->addStretch();
    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet(Theme::mutedText());
    connLayout->addWidget(m_statusLabel);

    layout->addLayout(connLayout);

    // 路径导航栏
    auto* navLayout = new QHBoxLayout;
    auto* upBtn = new QPushButton("↑ 上级");
    connect(upBtn, &QPushButton::clicked, this, &FileManagerWidget::onUpClicked);
    navLayout->addWidget(upBtn);

    m_pathEdit = new QLineEdit;
    m_pathEdit->setPlaceholderText("远端路径");
    m_pathEdit->setReadOnly(true);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this]() {
        if (m_sftp) navigateTo(m_pathEdit->text());
    });
    navLayout->addWidget(m_pathEdit, 1);

    auto* refreshBtn = new QPushButton("刷新");
    connect(refreshBtn, &QPushButton::clicked, this, &FileManagerWidget::onRefreshClicked);
    navLayout->addWidget(refreshBtn);

    layout->addLayout(navLayout);

    // 文件表格
    m_table = new QTableWidget;
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({"名称", "大小", "权限", "修改时间"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &FileManagerWidget::onItemDoubleClicked);
    layout->addWidget(m_table, 1);

    // 传输进度条
    m_progressLabel = new QLabel;
    m_progressLabel->setStyleSheet(Theme::mutedText());
    m_progressLabel->setVisible(false);
    layout->addWidget(m_progressLabel);

    m_progressBar = new QProgressBar;
    m_progressBar->setVisible(false);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(6);
    layout->addWidget(m_progressBar);

    // 操作按钮栏
    auto* actionsLayout = new QHBoxLayout;
    auto* downloadBtn = new QPushButton("下载");
    connect(downloadBtn, &QPushButton::clicked, this, &FileManagerWidget::onDownloadClicked);
    actionsLayout->addWidget(downloadBtn);

    auto* uploadBtn = new QPushButton("上传");
    connect(uploadBtn, &QPushButton::clicked, this, &FileManagerWidget::onUploadClicked);
    actionsLayout->addWidget(uploadBtn);

    auto* mkdirBtn = new QPushButton("新建文件夹");
    connect(mkdirBtn, &QPushButton::clicked, this, &FileManagerWidget::onMkdirClicked);
    actionsLayout->addWidget(mkdirBtn);

    auto* renameBtn = new QPushButton("重命名");
    connect(renameBtn, &QPushButton::clicked, this, &FileManagerWidget::onRenameClicked);
    actionsLayout->addWidget(renameBtn);

    auto* deleteBtn = new QPushButton("删除");
    connect(deleteBtn, &QPushButton::clicked, this, &FileManagerWidget::onDeleteClicked);
    actionsLayout->addWidget(deleteBtn);

    actionsLayout->addStretch();
    layout->addLayout(actionsLayout);
}

void FileManagerWidget::setConnectedUI(bool connected) {
    m_connectBtn->setEnabled(!connected);
    m_connCombo->setEnabled(!connected);
    m_disconnectBtn->setEnabled(connected);
    m_pathEdit->setReadOnly(!connected);
    if (!connected) {
        m_statusLabel->setText("未连接");
        m_table->setRowCount(0);
        m_pathEdit->clear();
    }
}

void FileManagerWidget::onConnectClicked() {
    if (m_connCombo->count() == 0) {
        QMessageBox::information(this, "提示",
            "没有可用的 SSH 连接，请先在「应用管理」中创建。");
        return;
    }

    QString connId = m_connCombo->currentData().toString();
    auto* conn = ConnectionManager::instance().getById(connId);
    if (!conn) return;

    m_currentConnId = connId;

    // 清理旧连接
    if (m_sftp) {
        m_sftp->disconnect();
        m_sftp->deleteLater();
        m_sftp = nullptr;
    }

    // 构造 SFTPClient 连接参数
    SFTPClient::ConnectParams params;
    params.host = conn->host;
    params.port = conn->port;
    params.username = conn->username;
    params.timeoutSec = 30;
    if (conn->authType == "key") {
        params.privateKeyPath = conn->privateKey;
    } else {
        params.password = conn->password;
    }

    m_sftp = new SFTPClient(this);
    connect(m_sftp, &SFTPClient::connected, this, &FileManagerWidget::onSftpConnected);
    connect(m_sftp, &SFTPClient::disconnected, this, &FileManagerWidget::onSftpDisconnected);
    connect(m_sftp, &SFTPClient::connectionError, this, &FileManagerWidget::onSftpError);
    connect(m_sftp, &SFTPClient::directoryListed, this, &FileManagerWidget::onDirectoryListed);
    connect(m_sftp, &SFTPClient::transferFinished, this, &FileManagerWidget::onTransferFinished);
    connect(m_sftp, &SFTPClient::operationFinished, this, &FileManagerWidget::onOperationFinished);

    m_statusLabel->setText("连接中...");
    ConnectionManager::instance().setActive(connId, true);
    m_sftp->connectTo(params);
    Logger::instance().info(QString("SFTP 连接请求: %1@%2:%3")
        .arg(conn->username, conn->host).arg(conn->port), "sftp");
}

void FileManagerWidget::onDisconnectClicked() {
    if (m_sftp) {
        m_sftp->disconnect();
    }
}

void FileManagerWidget::onSftpConnected() {
    setConnectedUI(true);
    m_statusLabel->setText("已连接");
    Logger::instance().success(QString("SFTP 已连接: %1").arg(m_currentConnId), "sftp");
    navigateTo(".");
}

void FileManagerWidget::onSftpDisconnected() {
    setConnectedUI(false);
    ConnectionManager::instance().setActive(m_currentConnId, false);
}

void FileManagerWidget::onSftpError(const QString& msg) {
    m_statusLabel->setText("连接失败");
    QMessageBox::warning(this, "连接错误", msg);
    ConnectionManager::instance().setActive(m_currentConnId, false);
}

void FileManagerWidget::onDirectoryListed(const QString& path,
                                           const QList<SFTPClient::FileEntry>& files) {
    Q_UNUSED(path);
    // 排序：目录在前，同类型按名称
    auto entries = files;
    std::sort(entries.begin(), entries.end(),
        [](const SFTPClient::FileEntry& a, const SFTPClient::FileEntry& b) {
            if (a.isDir != b.isDir) return a.isDir;
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });

    m_table->setRowCount(0);
    int row = 0;
    for (const auto& f : entries) {
        m_table->insertRow(row);

        QString displayName = f.isDir ? (f.name + "/") : f.name;
        auto* nameItem = new QTableWidgetItem(displayName);
        if (f.isDir) nameItem->setForeground(QColor("#4fc3f7"));
        nameItem->setData(Qt::UserRole, f.name);
        nameItem->setData(Qt::UserRole + 1, f.isDir);
        m_table->setItem(row, 0, nameItem);

        QString sizeStr;
        if (f.isDir) {
            sizeStr = "<DIR>";
        } else {
            sizeStr = formatBytes(f.size);
        }
        m_table->setItem(row, 1, new QTableWidgetItem(sizeStr));

        m_table->setItem(row, 2, new QTableWidgetItem(permsToOctal(f.permissions)));

        m_table->setItem(row, 3, new QTableWidgetItem(
            f.mtime.toString("yyyy-MM-dd HH:mm")));

        row++;
    }
    m_statusLabel->setText(QString("%1 项").arg(entries.size()));
}

void FileManagerWidget::onTransferFinished(const QString& path, bool success, const QString& error) {
    showTransferBar(false);
    Q_UNUSED(path);
    if (success) {
        m_statusLabel->setText("传输完成");
        Logger::instance().success("传输完成", "sftp");
        onRefreshClicked();
    } else {
        QMessageBox::warning(this, "传输失败", error);
        Logger::instance().error(QString("传输失败: %1").arg(error), "sftp");
    }
}

void FileManagerWidget::onOperationFinished(const QString& op, bool success, const QString& path) {
    Q_UNUSED(op);
    if (success) {
        Logger::instance().success(QString("操作成功: %1").arg(path), "sftp");
        onRefreshClicked();
    }
}

void FileManagerWidget::onItemDoubleClicked(int row, int) {
    auto* nameItem = m_table->item(row, 0);
    if (!nameItem) return;
    bool isDir = nameItem->data(Qt::UserRole + 1).toBool();
    QString name = nameItem->data(Qt::UserRole).toString();
    if (isDir) navigateTo(joinPath(m_currentPath, name));
}

void FileManagerWidget::onRefreshClicked() {
    if (!m_currentPath.isEmpty()) navigateTo(m_currentPath);
}

void FileManagerWidget::onUpClicked() {
    if (!m_currentPath.isEmpty() && m_currentPath != "/") {
        navigateTo(parentPath(m_currentPath));
    }
}

void FileManagerWidget::navigateTo(const QString& path) {
    if (!m_sftp) return;
    m_currentPath = path;
    m_pathEdit->setText(path);
    m_statusLabel->setText("加载中...");
    m_sftp->listDirectory(path);
}

QString FileManagerWidget::joinPath(const QString& base, const QString& name) const {
    if (base.endsWith("/")) return base + name;
    return base + "/" + name;
}

QString FileManagerWidget::parentPath(const QString& path) const {
    int idx = path.lastIndexOf('/');
    if (idx <= 0) return "/";
    return path.left(idx);
}

QString FileManagerWidget::selectedRemotePath() const {
    int row = m_table->currentRow();
    if (row < 0) return {};
    auto* item = m_table->item(row, 0);
    if (!item) return {};
    return joinPath(m_currentPath, item->data(Qt::UserRole).toString());
}

void FileManagerWidget::onDownloadClicked() {
    QString remote = selectedRemotePath();
    if (remote.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择要下载的文件");
        return;
    }
    int row = m_table->currentRow();
    if (row >= 0 && m_table->item(row, 0)->data(Qt::UserRole + 1).toBool()) {
        QMessageBox::information(this, "提示", "暂不支持下载目录，请选择文件");
        return;
    }

    QString suggested = QFileInfo(remote).fileName();
    QString local = QFileDialog::getSaveFileName(this, "下载到...", suggested);
    if (local.isEmpty()) return;

    showTransferBar(true, QString("下载: %1").arg(suggested));
    m_sftp->download(remote, local);
}

void FileManagerWidget::onUploadClicked() {
    if (!m_sftp) {
        QMessageBox::information(this, "提示", "请先连接服务器");
        return;
    }
    QString local = QFileDialog::getOpenFileName(this, "选择要上传的文件");
    if (local.isEmpty()) return;

    QString remote = joinPath(m_currentPath, QFileInfo(local).fileName());
    showTransferBar(true, QString("上传: %1").arg(QFileInfo(local).fileName()));
    m_sftp->upload(local, remote);
}

void FileManagerWidget::onDeleteClicked() {
    QString remote = selectedRemotePath();
    if (remote.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择要删除的项");
        return;
    }
    int row = m_table->currentRow();
    bool isDir = row >= 0 && m_table->item(row, 0)->data(Qt::UserRole + 1).toBool();

    auto ret = QMessageBox::question(this, "删除",
        QString("确定删除 %1「%2」吗？").arg(isDir ? "目录" : "文件").arg(remote));
    if (ret != QMessageBox::Yes) return;

    if (isDir) m_sftp->removeDirectory(remote);
    else m_sftp->removeFile(remote);
    // 稍等片刻再刷新（等待 SFTP 通知）
    QTimer::singleShot(200, this, &FileManagerWidget::onRefreshClicked);
}

void FileManagerWidget::onRenameClicked() {
    QString remote = selectedRemotePath();
    if (remote.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择要重命名的项");
        return;
    }
    QString oldName = QFileInfo(remote).fileName();
    bool ok = false;
    QString newName = QInputDialog::getText(this, "重命名", "新名称",
        QLineEdit::Normal, oldName, &ok);
    if (!ok || newName.isEmpty() || newName == oldName) return;

    m_sftp->rename(remote, joinPath(m_currentPath, newName));
}

void FileManagerWidget::onMkdirClicked() {
    if (!m_sftp) {
        QMessageBox::information(this, "提示", "请先连接服务器");
        return;
    }
    bool ok = false;
    QString name = QInputDialog::getText(this, "新建文件夹", "文件夹名称",
        QLineEdit::Normal, QString(), &ok);
    if (!ok || name.isEmpty()) return;
    m_sftp->makeDirectory(joinPath(m_currentPath, name));
}

void FileManagerWidget::showTransferBar(bool visible, const QString& label) {
    m_progressBar->setVisible(visible);
    m_progressLabel->setVisible(visible);
    if (visible) {
        m_progressLabel->setText(label);
        m_progressBar->setRange(0, 0);  // 不确定模式
    }
}