#pragma once

#include <QWidget>

class QLineEdit;
class QPushButton;
class QComboBox;
class QLabel;
class QTableWidget;
class QTableWidgetItem;

/**
 * DocumentWidget: 文档管理页面
 *
 * 浏览/上传/下载/搜索/删除本地附件库内容。
 * 数据由 AttachmentStore 单例提供。
 */
class DocumentWidget : public QWidget {
    Q_OBJECT

public:
    explicit DocumentWidget(QWidget* parent = nullptr);

protected:
    // 拖拽上传支持
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private slots:
    void onUploadClicked();
    void onUploadPaths(const QStringList& paths);
    void onDeleteClicked();
    void onDownloadClicked();
    void onOpenInFolderClicked();
    void onRowDoubleClicked(int row, int column);
    void onContextMenu(const QPoint& pos);
    void onFilterChanged();
    void onRefreshClicked();

private:
    void setupUI();
    void reloadTable();
    void updateStatusBar();
    void setTagsForSelection();

    QLineEdit*     m_searchEdit = nullptr;
    QComboBox*     m_sourceCombo = nullptr;     // 全部/手动/邮件
    QComboBox*     m_tagCombo = nullptr;        // 标签过滤
    QPushButton*   m_uploadBtn = nullptr;
    QPushButton*   m_deleteBtn = nullptr;
    QPushButton*   m_downloadBtn = nullptr;
    QPushButton*   m_openFolderBtn = nullptr;
    QPushButton*   m_setTagsBtn = nullptr;
    QPushButton*   m_refreshBtn = nullptr;
    QTableWidget*  m_table = nullptr;
    QLabel*        m_statusLabel = nullptr;

    QString m_currentFilterSource;   // "" / "manual" / "email"
    QString m_currentKeyword;
    QString m_currentTag;
};
