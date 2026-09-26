#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// AttachmentPreviewPane: 收件箱附件的内联预览面板
//
//   - show(filePath, mimeType)    加载文件并按类型渲染
//     * image/*        → QLabel + QPixmap 缩放显示（自适应宽度）
//     * text/*         → QPlainTextEdit 渲染（UTF-8 / Latin1）
//     * application/json, application/xml
//                     → QPlainTextEdit 高亮（语法不高亮，但等宽字体）
//     * 其他（如 PDF / Office） → 显示"暂不支持内联预览"占位 +
//                                [用默认应用打开] [在文件夹显示] 按钮
//
//   - clear()                     清空当前内容（回到正文预览）
//   - setExternalFallbackEnabled(bool)  控制是否显示 [打开] / [定位] 按钮
//
// 该面板被 MailContentPanel 作为 m_bodyStack 的一个新页面：
// 用户点击附件行 → MailWidget 触发下载 → 下载成功后调用本面板 show() →
// m_bodyStack->setCurrentWidget(previewPane)。底部 [返回正文] 按钮恢复。
// ─────────────────────────────────────────────────────────────────────────────

#include <QWidget>
#include <QString>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;

class AttachmentPreviewPane : public QWidget {
    Q_OBJECT
public:
    explicit AttachmentPreviewPane(QWidget* parent = nullptr);

    // 加载并预览一个本地文件
    void show(const QString& filePath, const QString& mimeType);

    // 清空预览（回到正文）
    void clear();

    // 当前文件路径（empty 表示无预览）
    QString currentPath() const { return m_path; }

signals:
    // 用户点 [返回正文] → 切回 m_previewView
    void backRequested();
    // 用户点 [用默认应用打开]
    void openExternallyRequested(const QString& filePath);
    // 用户点 [在文件夹显示]
    void revealRequested(const QString& filePath);

private slots:
    void onOpen();
    void onReveal();
    void onBack();

private:
    void buildUi();
    void renderUnsupported(const QString& filePath, const QString& mimeType);

    QString m_path;
    QString m_mimeType;

    QVBoxLayout*   m_root = nullptr;
    QLabel*        m_imageLabel = nullptr;   // 图片预览
    QPlainTextEdit* m_textEdit = nullptr;    // 文本预览
    QLabel*        m_unsupportedLabel = nullptr;  // 不支持类型占位
    QPushButton*   m_backBtn = nullptr;
    QPushButton*   m_openBtn = nullptr;
    QPushButton*   m_revealBtn = nullptr;
};