#include "document/DocumentWidget.h"
#include "document/AttachmentStore.h"
#include "app/Theme.h"
#include "core/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMenu>
#include <QInputDialog>

namespace {
QString humanSize(qint64 bytes) {
    const double kb = 1024.0;
    if (bytes < kb)        return QString("%1 B").arg(bytes);
    if (bytes < kb*kb)     return QString("%1 KB").arg(bytes / kb, 0, 'f', 1);
    if (bytes < kb*kb*kb)  return QString("%1 MB").arg(bytes / (kb*kb), 0, 'f', 1);
    return QString("%1 GB").arg(bytes / (kb*kb*kb), 0, 'f', 2);
}
}

DocumentWidget::DocumentWidget(QWidget* parent) : QWidget(parent) {
    setObjectName("DocumentWidget");
    setAcceptDrops(true);
    setupUI();

    // 监听存储变化（导入/删除后自动刷新）
    connect(&AttachmentStore::instance(), &AttachmentStore::changed,
            this, &DocumentWidget::onRefreshClicked);
    onRefreshClicked();
}

void DocumentWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    auto* header = new QLabel("文档管理");
    header->setStyleSheet(Theme::pageHeader());
    layout->addWidget(header);

    // ── 工具栏 ──
    auto* toolBar = new QHBoxLayout;
    toolBar->setSpacing(8);

    m_uploadBtn = new QPushButton("上传文件");
    m_uploadBtn->setMinimumHeight(32);
    connect(m_uploadBtn, &QPushButton::clicked, this, &DocumentWidget::onUploadClicked);
    toolBar->addWidget(m_uploadBtn);

    m_downloadBtn = new QPushButton("下载");
    m_downloadBtn->setMinimumHeight(32);
    connect(m_downloadBtn, &QPushButton::clicked, this, &DocumentWidget::onDownloadClicked);
    toolBar->addWidget(m_downloadBtn);

    m_openFolderBtn = new QPushButton("打开存储目录");
    m_openFolderBtn->setMinimumHeight(32);
    connect(m_openFolderBtn, &QPushButton::clicked, this, &DocumentWidget::onOpenInFolderClicked);
    toolBar->addWidget(m_openFolderBtn);

    m_setTagsBtn = new QPushButton("设置标签");
    m_setTagsBtn->setMinimumHeight(32);
    connect(m_setTagsBtn, &QPushButton::clicked, this, &DocumentWidget::setTagsForSelection);
    toolBar->addWidget(m_setTagsBtn);

    m_deleteBtn = new QPushButton("删除");
    m_deleteBtn->setMinimumHeight(32);
    connect(m_deleteBtn, &QPushButton::clicked, this, &DocumentWidget::onDeleteClicked);
    toolBar->addWidget(m_deleteBtn);

    toolBar->addStretch();

    m_refreshBtn = new QPushButton("刷新");
    m_refreshBtn->setMinimumHeight(32);
    connect(m_refreshBtn, &QPushButton::clicked, this, &DocumentWidget::onRefreshClicked);
    toolBar->addWidget(m_refreshBtn);

    layout->addLayout(toolBar);

    // ── 过滤器 ──
    auto* filterBar = new QHBoxLayout;
    filterBar->setSpacing(8);

    auto* srcLabel = new QLabel("来源:");
    filterBar->addWidget(srcLabel);
    m_sourceCombo = new QComboBox;
    m_sourceCombo->addItem("全部", "");
    m_sourceCombo->addItem("手动上传", "manual");
    m_sourceCombo->addItem("邮件附件", "email");
    connect(m_sourceCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &DocumentWidget::onFilterChanged);
    filterBar->addWidget(m_sourceCombo);

    auto* tagLabel = new QLabel("标签:");
    filterBar->addWidget(tagLabel);
    m_tagCombo = new QComboBox;
    m_tagCombo->addItem("全部", "");
    m_tagCombo->setMinimumWidth(120);
    connect(m_tagCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &DocumentWidget::onFilterChanged);
    filterBar->addWidget(m_tagCombo);

    auto* searchLabel = new QLabel("搜索:");
    filterBar->addWidget(searchLabel);
    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText("按文件名、标签或描述模糊搜索");
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { onFilterChanged(); });
    filterBar->addWidget(m_searchEdit, 1);

    layout->addLayout(filterBar);

    // ── 表格 ──
    m_table = new QTableWidget(0, 8);
    m_table->setHorizontalHeaderLabels(
        {"文件名", "来源", "大小", "标签", "描述", "原始 Sha256", "上传时间", "ID"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setColumnHidden(5, true);  // sha256 不显示
    m_table->setColumnHidden(7, true);  // id 不显示
    connect(m_table, &QTableWidget::cellDoubleClicked,
            this, &DocumentWidget::onRowDoubleClicked);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &DocumentWidget::onContextMenu);
    layout->addWidget(m_table, 1);

    m_statusLabel = new QLabel;
    m_statusLabel->setStyleSheet(Theme::mutedText());
    layout->addWidget(m_statusLabel);

    updateStatusBar();
}

void DocumentWidget::reloadTable() {
    if (!m_table) return;
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    AttachmentStore::Filter f;
    f.source  = m_currentFilterSource;
    f.keyword = m_currentKeyword;
    f.tag     = m_currentTag;

    const auto items = AttachmentStore::instance().list(f);
    m_table->setRowCount(items.size());
    int row = 0;
    for (const auto& a : items) {
        auto* nameItem = new QTableWidgetItem(a.originalName);
        nameItem->setData(Qt::UserRole, a.id);

        QString srcText;
        if (a.source == "manual") srcText = "手动上传";
        else if (a.source == "email") srcText = QString("邮件 (%1)").arg(a.sourceRef);
        else srcText = a.source;

        auto* srcItem  = new QTableWidgetItem(srcText);
        auto* sizeItem = new QTableWidgetItem(humanSize(a.size));
        sizeItem->setData(Qt::UserRole, a.size);
        auto* tagItem  = new QTableWidgetItem(a.tags);
        auto* descItem = new QTableWidgetItem(a.description);
        auto* shaItem  = new QTableWidgetItem(a.sha256);
        auto* timeItem = new QTableWidgetItem(
            a.uploadedAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"));

        QString idStr = QString::number(a.id);
        auto* idItem  = new QTableWidgetItem(idStr);

        m_table->setItem(row, 0, nameItem);
        m_table->setItem(row, 1, srcItem);
        m_table->setItem(row, 2, sizeItem);
        m_table->setItem(row, 3, tagItem);
        m_table->setItem(row, 4, descItem);
        m_table->setItem(row, 5, shaItem);
        m_table->setItem(row, 6, timeItem);
        m_table->setItem(row, 7, idItem);
        ++row;
    }
    m_table->setSortingEnabled(true);
    updateStatusBar();
}

void DocumentWidget::updateStatusBar() {
    auto& s = AttachmentStore::instance();
    int n = s.count();
    qint64 bytes = s.totalSize();
    if (m_statusLabel) {
        m_statusLabel->setText(QString("共 %1 个文件，占用 %2")
            .arg(n).arg(humanSize(bytes)));
    }

    // 刷新标签下拉
    if (m_tagCombo) {
        const QString prev = m_currentTag;
        m_tagCombo->blockSignals(true);
        m_tagCombo->clear();
        m_tagCombo->addItem("全部", "");
        for (const QString& t : s.allTags()) {
            m_tagCombo->addItem(t, t);
        }
        int idx = m_tagCombo->findData(prev);
        m_tagCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        m_tagCombo->blockSignals(false);
    }
}

void DocumentWidget::onFilterChanged() {
    m_currentFilterSource = m_sourceCombo ? m_sourceCombo->currentData().toString() : QString();
    m_currentKeyword      = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    m_currentTag          = m_tagCombo ? m_tagCombo->currentData().toString() : QString();
    reloadTable();
}

void DocumentWidget::onRefreshClicked() {
    reloadTable();
}

void DocumentWidget::onUploadClicked() {
    QStringList paths = QFileDialog::getOpenFileNames(this, "选择文件", {}, "所有文件 (*.*)");
    if (!paths.isEmpty()) onUploadPaths(paths);
}

void DocumentWidget::onUploadPaths(const QStringList& paths) {
    int ok = 0, fail = 0;
    for (const QString& p : paths) {
        if (!QFile::exists(p)) { ++fail; continue; }
        QString err;
        qint64 id = AttachmentStore::instance().importFile(p, "manual", {}, {}, {}, {}, &err);
        if (id > 0) ++ok;
        else { ++fail; Logger::instance().error(QString("导入失败 %1: %2").arg(p, err), "doc"); }
    }
    if (fail > 0) {
        QMessageBox::warning(this, "上传",
            QString("已导入 %1 个文件，%2 个失败（详见日志）").arg(ok).arg(fail));
    }
}

void DocumentWidget::onDeleteClicked() {
    if (!m_table) return;
    const auto selected = m_table->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择要删除的文件");
        return;
    }
    if (QMessageBox::question(this, "确认",
            QString("确定删除选中的 %1 个文件吗？\n（无引用的物理文件也会一并删除）").arg(selected.size()))
        != QMessageBox::Yes) return;

    int ok = 0;
    for (const auto& idx : selected) {
        qint64 id = m_table->item(idx.row(), 0)->data(Qt::UserRole).toLongLong();
        QString err;
        if (AttachmentStore::instance().remove(id, &err)) ++ok;
        else Logger::instance().error(QString("删除失败 id=%1: %2").arg(id).arg(err), "doc");
    }
    Logger::instance().info(QString("删除 %1/%2 个文件").arg(ok).arg(selected.size()), "doc");
}

void DocumentWidget::onDownloadClicked() {
    if (!m_table) return;
    auto sel = m_table->selectionModel()->selectedRows();
    if (sel.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择要下载的文件");
        return;
    }
    for (const auto& idx : sel) {
        qint64 id = m_table->item(idx.row(), 0)->data(Qt::UserRole).toLongLong();
        auto a = AttachmentStore::instance().getById(id);
        if (a.id == 0) continue;
        QString dest = QFileDialog::getSaveFileName(this, "另存为", a.originalName);
        if (dest.isEmpty()) continue;
        QString err;
        if (!AttachmentStore::instance().exportTo(id, dest, &err)) {
            QMessageBox::warning(this, "下载失败", err);
        }
    }
}

void DocumentWidget::onOpenInFolderClicked() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(AttachmentStore::instance().storageDir()));
}

void DocumentWidget::onRowDoubleClicked(int row, int /*col*/) {
    if (!m_table) return;
    qint64 id = m_table->item(row, 0)->data(Qt::UserRole).toLongLong();
    auto a = AttachmentStore::instance().getById(id);
    if (a.id == 0) return;
    QString absPath = AttachmentStore::instance().storageDir() + "/" + a.storedPath;
    if (!QFile::exists(absPath)) {
        QMessageBox::warning(this, "提示", "物理文件不存在: " + absPath);
        return;
    }
    // 用系统默认程序打开（Windows 走 ShellExecute）
    QDesktopServices::openUrl(QUrl::fromLocalFile(absPath));
}

void DocumentWidget::onContextMenu(const QPoint& pos) {
    if (!m_table) return;
    QMenu menu(this);
    menu.addAction("下载/另存为", this, &DocumentWidget::onDownloadClicked);
    menu.addAction("在文件夹中显示", [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(AttachmentStore::instance().storageDir()));
    });
    menu.addAction("设置标签...", this, &DocumentWidget::setTagsForSelection);
    menu.addSeparator();
    menu.addAction("删除", this, &DocumentWidget::onDeleteClicked);
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void DocumentWidget::setTagsForSelection() {
    if (!m_table) return;
    auto sel = m_table->selectionModel()->selectedRows();
    if (sel.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择文件");
        return;
    }
    bool ok;
    QString text = QInputDialog::getText(this, "设置标签",
        "标签（多个用逗号分隔，留空清除）:",
        QLineEdit::Normal, {}, &ok);
    if (!ok) return;

    auto& store = AttachmentStore::instance();
    int updated = 0;
    for (const auto& idx : sel) {
        qint64 id = m_table->item(idx.row(), 0)->data(Qt::UserRole).toLongLong();
        QString err;
        if (store.updateTags(id, text.trimmed(), &err)) ++updated;
        else Logger::instance().error(QString("更新标签失败 id=%1: %2").arg(id).arg(err), "doc");
    }
    Logger::instance().info(QString("更新 %1 个附件的标签").arg(updated), "doc");
}

void DocumentWidget::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void DocumentWidget::dropEvent(QDropEvent* e) {
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls()) {
        QString p = u.toLocalFile();
        if (!p.isEmpty() && QFile::exists(p)) paths << p;
    }
    if (!paths.isEmpty()) {
        onUploadPaths(paths);
        e->acceptProposedAction();
    }
}
